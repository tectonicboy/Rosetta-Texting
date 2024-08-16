#include <errno.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "cryptolib.h"
#include "coreutil.h"

#define SERVER_PORT    54746
#define PRIVKEY_BYTES  40   
#define PUBKEY_LEN     384
#define MAX_CLIENTS    64
#define MAX_PEND_MSGS  1024
#define MAX_CHATROOMS  64
#define MAX_MSG_LEN    1024
#define MAX_SOCK_QUEUE 1024
#define MAX_BIGINT_SIZ 12800
#define MAGIC_LEN      8 
#define TEMP_BUF_SIZ   16384

#define SIGNATURE_LEN  ((2 * sizeof(bigint)) + (2 * PRIVKEY_BYTES))

#define MAGIC_00 0xAD0084FF0CC25B0E
#define MAGIC_01 0xE7D09F1FEFEA708B
#define MAGIC_02 0x146AAE4D100DAEEA

/* A bitmask for various control-related purposes.
 * 
 * Currently used bits:
 *
 *  [0] - Whether the temporary login handshake memory region is locked or not.
 *        This memory region holds very short-term public/private keys used
 *        to transport the client's long-term public key to us securely.
 *        It can't be local, because the handshake spans several transmissions,
 *        (thus is interruptable) yet needs the keys for its entire duration.
 *        Every login procedure needs it. If a second client attempts to login
 *        while another client is already logging in, without checking this bit,
 *        the other client's login procedure's short-term keys could be erased.
 *        Thus, use this bit to disallow more than 1 login handshake at a time.
 */ 
u32 server_control_bitmask = 0;

/* A bitmask telling the server which client slots are currently free.   */
u64 clients_status_bitmask = 0;

u32 next_free_user_ix = 0;
u32 next_free_room_ix = 1;

u8 server_privkey[PRIVKEY_BYTES];

struct connected_client{
    u32 room_ix;
    u32 num_pending_msgs;
    u8* pending_msgs[MAX_PEND_MSGS];
    u8* client_pubkey;
    u64 pubkey_siz_bytes;
};

struct chatroom{
    u32 num_people;
    u32 owner_ix;
    char*    room_name;
};

struct connected_client clients[MAX_CLIENTS];
struct chatroom rooms[MAX_CHATROOMS];

/* Memory region holding the temporary keys for the login handshake. */
u8* temp_handshake_buf;

/* Linux Sockets API related globals. */
int port = SERVER_PORT
   ,listening_socket
   ,optval1 = 1
   ,optval2 = 2
   ,client_socket_fd;
      
socklen_t clientLen = sizeof(struct sockaddr_in);

struct bigint *M, *Q, *G, *Gm, server_privkey_bigint;
struct sockaddr_in client_address;
struct sockaddr_in server_address;

/* First thing done when we start the server - initialize it. */
u32 self_init(){

    /* Allocate memory for the temporary login handshake memory region. */
    temp_handshake_buf = calloc(1, TEMP_BUF_SIZ);

    server_address = {  .sin_family = AF_INET
                       ,.sin_port = htons(port)
                       ,.sin_addr.s_addr = INADDR_ANY
                     };
                                                 
    if( (listening_socket = socket(AF_INET, SOCK_STREAM, 0)) == -1){
        printf("[ERR] Server: Could not open server socket. Aborting.\n");
        return 1;
    }
    
    setsockopt(
          listening_socket, SOL_SOCKET, SO_REUSEPORT, &optval1, sizeof(optval1)
    );  
      
    setsockopt(
          listening_socket, SOL_SOCKET, SO_REUSEADDR, &optval2, sizeof(optval2)
    );
                      
    if( 
        (
         bind(
            listening_socket
           ,(struct sockaddr*)&server_address
           ,sizeof(server_address)
        )
      ) == -1
    )
    {
        if(errno != 13){
            printf("[ERR] Server: bind() failed. Errno != 13. Aborting.\n");
            return 1;
        }
    }
       
    if( (listen(listening_socket, MAX_SOCK_QUEUE)) == -1){
        printf("[ERR] Server: couldn't begin listening. Aborting.\n");
        return 1;
    }
    
    /*  Server will use its private key to compute Schnorr signatures of 
     *  everything it transmits, so all users can verify it with the server's
     *  public key they already have by default for authenticity.
     */
    FILE* privkey_dat = fopen("server_privkey.dat", "r");
    
    if(!privkey_dat){
        printf("[ERR] Server: couldn't open private key DAT file. Aborting.\n");
        return 1;
    }
    
    if(fread(server_privkey, 1, PRIVKEY_BYTES, privkey_dat) != PRIVKEY_BYTES){
        printf("[ERR] Server: couldn't get private key from file. Aborting.\n");
        return 1;
    }
    else{
        printf("[OK]  Server: Successfully loaded private key.\n");
    }
    
    /* Initialize the BigInt that stores the server's private key. */
    bigint_create(&server_privkey_bigint, MAX_BIGINT_SIZ, 0);
    
    memcpy(server_privkey_bigint.bits, server_privkey, PRIVKEY_BYTES); 
    
    server_privkey_bigint.used_bits = 
                            get_used_bits(server_privkey, PRIVKEY_BYTES);
                            
    server_privkey_bigint.free_bits = 
                            MAX_BIGINT_SIZ - server_privkey_bigint.used_bits;
            
    /* Load in other BigInts needed for the cryptography to work. */
    
    /* Diffie-Hellman modulus M, 3071-bit prime number */                        
    M = get_BIGINT_from_DAT
        (3072, "../saved_nums/M_raw_bytes.dat\0", 3071, RESBITS);
    
    /* 320-bit prime exactly dividing M-1, making M cryptographycally strong. */
    Q = get_BIGINT_from_DAT
        (320,  "../saved_nums/Q_raw_bytes.dat\0", 320,  RESBITS);
    
    /* Diffie-Hellman generator G = G = 2^((M-1)/Q) */
    G = get_BIGINT_from_DAT
        (3072, "../saved_nums/G_raw_bytes.dat\0", 3071, RESBITS);

    /* Montgomery Form of G, since we use Montgomery Multiplication. */
    Gm = get_BIGINT_from_DAT
        (3072, "../saved_nums/PRACTICAL_Gmont_raw_bytes.dat\0", 3071, RESBITS);
    
    fclose(privkey_dat);
    
    return 0;
}

u8 check_pubkey_exists(u8* pubkey_buf, u64 pubkey_siz){

    if(pubkey_siz < 300){
        printf("\n[ERR] Server: Passed a small PubKey Size: %u\n", pubkey_siz);
        return 2;
    }

    /* client slot has to be taken, size has to match, then pubkey can match. */
    for(u64 i = 0; i < MAX_CLIENTS; ++i){
        if(   (clients_status_bitmask & (1ULL << (63ULL - i)))
           && (clients[i].pubkey_siz_bytes == pubkey_siz)
           && (memcmp(pubkey_buf, clients[i].client_pubkey, pubkey_siz) == 0)
          )
        {
            printf("\n[ERR] Server: PubKey already exists.\n\n");
            return 1;
        }
    }
    
    return 0;
}

/* A client requested to be logged in Rosetta */
__attribute__ ((always_inline)) 
inline
void process_msg_00(u8* msg_buf){

    bigint *A_s
          ,zero
          ,Am
          ,*b_s
          ,*B_s
          ,*X_s;
            
    u32  *KAB_s
        ,*KBA_s
        ,*Y_s
        ,*N_s
        ,tempbuf_byte_offset = 0
        ,replybuf_byte_offset = 0;
        
    u8 *signature_buf = calloc(1, SIGNATURE_LEN);
            
    u8* reply_buf;
    u64 reply_len;

    reply_len = (3 * MAGIC_LEN) + SIGNATURE_LEN + PUBKEY_LEN;
    
    reply_buf = calloc(1, reply_len);

    /* Construct a bigint out of the client's short-term public key.          */
    /* Here's where a constructor from a memory buffer and its length is good */
    /* Find time to implement one as part of the BigInt library.              */
    
    /* Allocate any short-term keys and other cryptographic artifacts needed for
     * the initial login handshake protocol in the designated memory region and
     * lock it, disallowing another parallel login attempt to corrupt them.
     */
    server_control_bitmask |= (1ULL << 63ULL);
    
    A_s = temp_handshake_buf;
    A_s->bits = calloc(1, MAX_BIGINT_SIZ);
    memcpy(A_s->bits, msg_buf + 16, *(msg_buf + 8));
    A_s->bits_size = MAX_BIGINT_SIZ;
    A_s->used_bits = get_used_bits(msg_buf + 16, (u32)*(msg_buf + 8));
    A_s->free_bits = A_s->bits_size - A_s->used_bits;
    
    /* Check that (0 < A_s < M) and that (A_s^(M/Q) mod M = 1) */
    
    /* A "check non zero" function in the BigInt library would also be useful */
    
    bigint_create(&zero, MAX_BIGINT_SIZ, 0);
    bigint_create(&Am,   MAX_BIGINT_SIZ, 0);
    
    Get_Mont_Form(A_s, &Am, M);
    
    if(   ((bigint_compare2(&zero, A_s)) != 3) 
        || 
          ((bigint_compare2(M, A_s)) != 1)
        ||
          (check_pubkey_form(Am, M, Q) == 0) 
      )
    {
        printf("[ERR] Server: Client's short-term public key is invalid.\n");
        printf("\n\nIts info and ALL bits:\n\n");
        bigint_print_info(A_s);
        bigint_print_all_bits(A_s);
        goto label_cleanup;
    } 
    
    /*  Server generates its own short-term DH keys and a shared secret X:
     *    
     *       b_s = random in the range [1, Q)
     * 
     *       B_s = G^b_s mod M     <--- Montgomery Form of G used.
     *   
     *       X_s = A_s^b_s mod M   <--- Montgomery Form of A_s used.
     *
     *  Server extracts two keys and two values Y, N from byte regions in X:
     *
     *       KAB_s = X_s[0  .. 31 ]
     *       KBA_s = X_s[32 .. 63 ]
     *       Y_s   = X_s[64 .. 95 ]
     *       N_s   = X_s[96 .. 107] <-- 12-byte Nonce for ChaCha20.
     *
     *  These 7 things are all stored in the designated locked memory region.
     */

    gen_priv_key(PRIVKEY_BYTES, (temp_handshake_buf + sizeof(bigint)));
    
    b_s = *(temp_handshake_buf + sizeof(bigint));
    
    /* Interface generating a pub_key still needs priv_key in a file. Change. */
    save_BIGINT_to_DAT("temp_privkey_DAT\0", b_s);
  
    B_s = gen_pub_key(PRIVKEY_BYTES, "temp_privkey_DAT\0", MAX_BIGINT_SIZ);
    
    /* Place the server short-term pub_key also in the locked memory region. */
    memcpy((temp_handshake_buf + (2 * sizeof(bigint))), B_s, sizeof(bigint));
    
    /* X_s = A_s^b_s mod M */
    X_s = temp_handshake_buf + (3 * sizeof(bigint));
    
    bigint_create(X_s, MAX_BIGINT_SIZ, 0);
    
    MONT_POW_modM(Am, b_s, M, X_s);
    
    /* Extract KAB_s, KBA_s, Y_s and N_s into the locked memory region. */
    tempbuf_byte_offset = 4 * sizeof(bigint);
    memcpy(temp_handshake_buf + tempbuf_byte_offset, X_s->bits +  0, 32);
    KAB_s = (u32*)(temp_handshake_buf + tempbuf_byte_offset);
    
    tempbuf_byte_offset += 32;
    memcpy(temp_handshake_buf + tempbuf_byte_offset, X_s->bits + 32, 32);
    KBA_s = (u32*)(temp_handshake_buf + tempbuf_byte_offset);
    
    tempbuf_byte_offset += 32;
    memcpy(temp_handshake_buf + tempbuf_byte_offset, X_s->bits + 64, 32);
    Y_s = temp_handshake_buf + tempbuf_byte_offset;
        
    tempbuf_byte_offset += 32;
    memcpy(temp_handshake_buf + tempbuf_byte_offset, X_s->bits + 96, 12);
    N_s = temp_handshake_buf + tempbuf_byte_offset;
    
    /*  Compute a signature of Y_s using LONG-TERM private key b, yielding SB. */
    Signature_GENERATE
      (M, Q, Gm, Y_s, 32, signature_buf, &server_privkey_bigint, PRIVKEY_BYTES);
                  
    /* Server sends in the clear (B_s, SB) to the client. */
    
    /* Find time to change the signature generation to only place the actual
     * bits of s and e, excluding their bigint structs, because we reconstruct
     * their bigint structs easily with get_used_bits().
     */
    
    /* Construct the reply buffer. */   
     replybuf_byte_offset = 0;
    *((u64*)(reply_buf + replybuf_byte_offset)) = (u64)MAGIC_00;
    
    replybuf_byte_offset += MAGIC_LEN;
    *((u64*)(reply_buf + replybuf_byte_offset)) = (u64)PUBKEY_LEN; 
    
    replybuf_byte_offset += MAGIC_LEN;
    memcpy(reply_buf + replybuf_byte_offset, B_s->bits, PUBKEY_LEN);
    
    replybuf_byte_offset += PUBKEY_LEN;
    *((u64*)(reply_buf + replybuf_byte_offset)) = (u64)SIGNATURE_LEN; 
    
    replybuf_byte_offset += MAGIC_LEN;
    memcpy(reply_buf + replybuf_byte_offset, signature_buf, SIGNATURE_LEN);
    
    /* Send the reply back to the client. */
    if(send(client_socket, reply_buf, reply_len, 0) == -1){
        printf("[ERR] Server: Couldn't reply with MAGIC_00 msg type.\n");
    }
    else{
        printf("[OK]  Server: Replied to client with MAGIC_00 msg type.\n");
    }
      
label_cleanup: 

    /* Free temporaries on the heap. */
    free(zero.bits);
    free(Am.bits);
    free(signature_buf);
    free(reply_buf);
  
    return;
}

/* Second part of the initial login handshake */
__attribute__ ((always_inline)) 
inline
void process_msg_01(u8* msg_buf){

    const u64 B = 64;
    const u64 L = 128;
    const u64 magic02 = MAGIC_02;
    const u64 maigc01 = MAGIC_01;
    
    u8* K0   = calloc(1, B);
    u8* ipad = calloc(1, B);
    u8* opad = calloc(1, B);
    u8* K0_XOR_ipad_TEXT = calloc(1, (B + PUBKEY_LEN));
    u8* BLAKE2B_output = calloc(1, L);   
    u8* last_BLAKE2B_input = calloc(1, (B + L));
    u8* K0_XOR_ipad = calloc(1, B);
    u8* K0_XOR_opad = calloc(1, B);
    u8* HMAC_output = calloc(1, 8);
    u8* client_pubkey_buf = calloc(1, PUBKEY_LEN);
    
    u8* reply_buf;  
    
    memset(opad, 0x5c, B);
    memset(ipad, 0x36, B);
    
    /*  Use what's already in the locked memory region to compute HMAC and 
     *  to decrypt the user's long-term public key
     *  Server uses KAB_s to compute the same HMAC on A_x
     *
     *  Server uses KAB_s to compute the same HMAC on A_x as the client did. 
     *
     *  HMAC parameters here:
     *
     *  B    = input block size in bytes of BLAKE2B = 64
     *  H    = hash function to be used - unkeyed BLAKE2B
     *  ipad = buffer of the 0x36 byte repeated B=64 times
     *  K    = key KAB_s
     *  K_0  = K after pre-processing to form a B=64-byte key.
     *  L    = output block size in bytes of BLAKE2B = 128
     *  opad = buffer of the 0x5c byte repeated B=64 times
     *  text = A_x
     */ 
     
    /* Step 3 of HMAC construction */
    /* Length of K is less than B so append 0s to it until it's long enough. */
    memcpy(K0 + 32, temp_handshake_buf + (4 * sizeof(bigint)), 32);

    /* Step 4 of HMAC construction */
    for(u64 i = 0; i < B; ++i){
        K0_XOR_ipad[i] = (K0[i] ^ ipad[i]);
    }
    
    /* step 5 of HMAC construction */
    
    memcpy(K0_XOR_ipad_TEXT, K0_XOR_ipad, B);
    memcpy(K0_XOR_ipad_TEXT + B, msg_buf + (2 * MAGIC_LEN), PUBKEY_LEN);
    
    /* step 6 of HMAC construction */

    /* Call BLAKE2B on K0_XOR_ipad_TEXT */
  
    BLAKE2B_INIT(K0_XOR_ipad_TEXT, B + PUBKEY_LEN, 0, L, BLAKE2B_output);
    
    /* Step 7 of HMAC construction */
    for(u64 i = 0; i < B; ++i){
        K0_XOR_opad[i] = (K0[i] ^ opad[i]);
    }
    
    /* Step 8 of HMAC construction */
    
    /* Combine first BLAKE2B output buffer with K0_XOR_opad. */
    /* B + L bytes total length */

    memcpy(last_BLAKE2B_input, K0_XOR_opad, B);
    memcpy(last_BLAKE2B_input + B, BLAKE2B_output, L);
    
    /* Step 9 of HMAC construction */
    
    /* Call BLAKE2B on the combined buffer in step 8. */
    BLAKE2B_INIT(last_BLAKE2B_input, B + L, 0, L, BLAKE2B_output);
    
    /* Take the 8 leftmost bytes to form the HMAC output. */
    memcpy(HMAC_output, BLAKE2B_output, 8);
    
    /* Now compare calculated HMAC with the HMAC the client sent us */
    for(u64 i = 0; i < 8; ++i){
        if(HMAC_output[i] != msg_buf[PUBKEY_LEN + (2*MAGIC_LEN) + i]){
            printf("[ERR] Server: HMAC authentication codes don't match!\n\n");
            printf("[OK]  Server: Discarding transmission.\n");
            goto label_cleanup;
        }
    }
    
    /*
     *  Server uses KAB_s as key and 12-byte N_s as Nonce in ChaCha20 to
     *  decrypt A_x, revealing the client's long-term DH public key A.
     *
     *  Server then destroys all cryptographic artifacts for handshake. 
     */
     CHACHA20(msg_buf + (2*MAGIC_LEN)
             ,PUBKEY_LEN
             ,temp_handshake_buf + ((4*sizeof(bigint)) + (3*32))
             ,3
             ,temp_handshake_buf + (4 * sizeof(bigint))
             ,8
             ,client_pubkey_buf
             );
             
    /* Now we have the decrypted client's long-term public key! */
     
 
    /* if a message arrived to permit a newly arrived user to use Rosetta, but
     * currently the maximum number of clients are using it -> Try later.
     */
    if(next_free_user_ix == MAX_CLIENTS){
        printf("[ERR] Server: Not enough client slots to let a user in.\n");
        printf("              Letting the user know and to try later.  \n");
        
        /* Construct the ROSETTA FULL reply msg buffer */
        reply_len = (2*8) + SIGNATURE_LEN;
        reply_buf = calloc(1, reply_len);
    
        *((u64*)(reply_buf + 0)) = MAGIC_02;
        *((u64*)(reply_buf + 8)) = SIGNATURE_LEN;
        
        Signature_GENERATE
        (M,Q,Gm,&magic02,8,(reply_buf+16),&server_privkey_bigint,PRIVKEY_BYTES);
        
        if(send(client_socket, reply_buf, reply_len, 0) == -1){
            printf("[ERR] Server: Couldn't send full-rosetta message.\n");
        }
        else{
            printf("[OK]  Server: Told client Rosetta is full, try later\n");
        }
        goto label_cleanup;
    }
    
    if( (check_pubkey_exists(client_pubkey_buf, PUBKEY_LEN)) > 0 ){
        printf("[ERR] Server: Obtained login public key already exists.\n");
        printf("\n[OK]  Server: Discarding transmission.\n");
        goto label_cleanup;
    }
    
    /* Construct the login OK reply msg buffer. */
    /* It will contain the user ID */
    /* Encrypt the ID with chacha20 and KBA key and N_s nonce! */
    
    /* Try using a chacha counter even with less than 64 bytes of input. */
    
    reply_len  = (3*8) + SIGNATURE_LEN;
    reply_buf  = calloc(1, reply_len);
    
    CHACHA20(&next_free_user_ix
             ,8
             ,temp_handshake_buf + ((4*sizeof(bigint)) + (3*32))
             ,3
             ,temp_handshake_buf + ((4*sizeof(bigint)) + (1*32))
             ,8
             ,(reply_buf + 8)
             );
             
    *((u64*)(reply_buf +  0)) = MAGIC_01;
    *((u64*)(reply_buf + 16)) = SIGNATURE_LEN;
    
    Signature_GENERATE
        (M,Q,Gm,&magic01,8,(reply_buf+24),&server_privkey_bigint,PRIVKEY_BYTES);
    
    /* Server bookkeeping - populate this user's slot, find next free slot. */
  
    clients[next_free_user_ix].room_ix = 0;
    clients[next_free_user_ix].num_pending_msgs = 0;

    for(size_t i = 0; i < MAX_PEND_MSGS; ++i){
        clients[next_free_user_ix].pending_msgs[i] = calloc(1, MAX_MSG_LEN);
    }
    
    clients[next_free_user_ix].pubkey_siz_bytes = PUBKEY_LEN;


    clients[next_free_user_ix].client_pubkey 
     = 
     calloc(1, clients[next_free_user_ix].pubkey_siz_bytes);
     
    memcpy(  clients[next_free_user_ix].client_pubkey 
             ,client_pubkey_buf
             ,clients[next_free_user_ix].pubkey_siz_bytes
           );
    
    /* Reflect the new taken user slot in the global user status bitmask. */
    clients_status_bitmask |= (1ULL << (63ULL - next_free_user_ix));
    
    /* Increment it one space to the right, since we're guaranteeing by
     * logic in the user erause algorithm that we're always filling in
     * a new user in the LEFTMOST possible empty slot.
     *
     * If you're less than (max_users), look at this slot and to the right
     * in the bitmask for the next leftmost empty user slot index. If you're
     * equal to (max_users) then the maximum number of users are currently
     * using Rosetta. Can't let any more people in until one leaves.
     *
     * Here you either reach MAX_CLIENTS, which on the next attempt to 
     * let a user in and fill out a user struct for them, will indicate
     * that the maximum number of people are currently using Rosetta, or
     * you reach a bit at an index less than MAX_CLIENTS that is 0 in the
     * global user slots status bitmask.
     */
    ++next_free_user_ix;
    
    while(next_free_user_ix < MAX_CLIENTS){
        if(!(clients_status_bitmask & (1ULL<<(63ULL - next_free_user_ix))))
        {
            break;
        }
        ++next_free_user_ix;
    }
                
    if(send(client_socket, reply_buf, reply_len, 0) == -1){
        printf("[ERR] Server: Couldn't send Login-OK message.\n");
        goto label_cleanup;
    }
    else{
        printf("[OK]  Server: Told client Login went OK, sent their index.\n");
    }
    
    printf("\n\n[OK]  Server: SUCCESS - Permitted a user in Rosetta!!\n\n");

label_cleanup:

    /* Now it's time to clear and unlock the temporary login memory region. */
    
    /* memset(temp_handshake_buf, 0x00, TEMP_BUF_SIZ); */
    
    /* This version of bzero() prevents the compiler from eliminating and 
     * optimizing away the call that clears the buffer if it determines it
     * to be "unnecessary". For security reasons, since this buffer contains
     * keys and other cryptographic artifacts that are meant to be extremely
     * short-lived, use this explicit version to prevent the compiler from 
     * optimizing the call away.
     */
    explicit_bzero(temp_handshake_buf, TEMP_BUF_SIZ);
    
    server_control_bitmask &= ~(1ULL << 63ULL);
    
    /* Free temporaries on the heap. */
    free(K0_buf);
    free(ipad);
    free(opad);
    free(K0_XOR_ipad_TEXT);
    free(BLAKE2B_output);   
    free(last_BLAKE2B_input);
    free(K0_XOR_ipad);
    free(K0_XOR_opad);
    free(HMAC_output);
    free(client_pubkey_buf);

    return ;
}

/* Client requested to create a new chatroom. */
__attribute__ ((always_inline)) 
inline
void process_msg_10(u8* msg_buf){
        


}

/*  To make the server design more elegant, this top-level message processor 
 *  only checks whether the message is legit or not, and which of the predefined
 *  accepted types it is.
 *
 *  The message is then sent to its own individual processor function to be
 *  further analyzed and reacted to as defined by the Security Scheme.
 *
 *  This logical split of functionality eases the server implementation.
 *
 *  Here is a complete list of possible legitimate transmissions to the server:
 *
 *      - A client decides to log in Rosetta:
 *
 *          - [TYPE_00]: Client sent its short-term pub_key in the clear.
 *          - [TYPE_01]: Client sent encrypted long-term pub_key + 8-byte HMAC
 *
 *      - A client decides to make a new chat room:
 *
 *          - [TYPE_10]: Client sent a request to create a new chatroom. 
 *
 *
 *      - A client decides to join a chat room.
 *      - A client decides to send a new message to the chatroom.
 *      - A client decides to poll the server about unreceived messages.
 *      - A client decides to exit the chat room they're in.
 *      - A client decides to log off Rosetta.
 */

u32 process_new_message(){

    u8* client_msg_buf = calloc(1, MAX_MSG_LEN);
    s64 bytes_read; 
    u64 transmission_type;
    u64 expected_siz;
    char msg_type_str = calloc(1, 3);
    msg_type_str[2] = '\0';
    
    
    /* Capture the message the Rosetta TCP client sent to us. */
    bytes_read = recv(client_socket, client_msg_buf, MAX_MSG_LEN, 0);
    
    if(bytes_read == -1 || bytes_read < 8){
        printf("[ERR] Server: Couldn't read message on socket or too short.\n");
        goto label_cleanup;
    }
    else{
        printf("[OK]  Server: Read %u bytes from a request!\n\n", bytes_read);
    }
           
    /* Read the first 8 bytes to see what type of init transmission it is. */
    transmission_type = *((u64*)client_msg_buf);
    
    switch(transmission_type){
    
    /* A client tried to log in Rosetta */
    case(MAGIC_00){
        
        /* Size must be in bytes: 8 + 8 + pubkeysiz, which is bytes[8-15] */
        expected_siz = (16 + (*((u64*)(client_msg_buf + 8))));
        strncpy(msg_type_str, "00", 2);
        
        if(bytes_read != expected_siz){
            goto label_error;
        }
        
        process_msg_00(client_msg_buf);
    }
    
    /* Login part 2 - client sent their encrypted long-term public key. */
    case(MAGIC_01){  

        /* Size must be in bytes: 8 + 8 + 8 + pubkey size at msg[8-15] */
        expected_siz = (16 + (*((u64*)(client_msg_buf + 8))) + 8);
        strncpy(msg_type_str, "01", 2); 
        
        if(bytes_read != expected_siz){           
            goto label_error;
        }
    
        /* If transmission is of a valid type and size, process it. */
        process_msg_01(client_msg_buf);            
    }
       
    } /* end switch */
    
label_error:

    printf("[ERR] Server: MSG Type was %s but of wrong size.\n"
           ,msg_type_str
          );
          
    printf("               Size was: %ld\n", bytes_read);
    printf("               Expected: %lu\n", expected_siz);
    printf("\n[OK]  Server: Discarding transmission.\n\n ");
        
        
label_cleanup: 

    /* FREE EVERYTHING THIS FUNCTION ALLOCATED */
    free(client_msg_buf);
    free(msg_type_str);
}
int main(){

    u32 status;
    
    /* Initialize Linux Sockets API, load cryptographic keys and artifacts. */ 
    status = self_init();
    
    if(status){
        printf("[ERR] Server: Could not initialize. Terminating.\n");
        return 1;
    }
    
    printf("\n\n[OK]  Server: SUCCESS - Finished initializing!\n\n");
    
    while(1){

        /* Block on this accept() call until someone sends us a message. */
        client_socket_fd = accept(  server_socket
                                   ,(struct sockaddr*)(&client_address)
                                   ,&clientLen
                                 );
                                   
        /* 0 on success, greater than 0 otherwise. */                         
        status = process_new_message();
        
        if(status){
            printf("\n\n****** WARNING ******\n\n"
                   "Error while processing a received "
                   "transmission, look at log to find it.\n"
                  );    
        }                      
    }
                 
    return 0;
}
