#include <errno.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "../lib/coreutil.h"

#define SERVER_PORT      54746
#define PRIVKEY_LEN      40   
#define PUBKEY_LEN       384
#define MAX_CLIENTS      64
#define MAX_PEND_MSGS    64
#define MAX_CHATROOMS    64
#define MAX_MSG_LEN      8192
#define MAX_TXT_LEN      1024
#define MAX_SOCK_QUEUE   1024
#define MAX_BIGINT_SIZ   12800
#define SMALL_FIELD_LEN  8 
#define TEMP_BUF_SIZ     16384
#define SESSION_KEY_LEN  32
#define ONE_TIME_KEY_LEN 32
#define INIT_AUTH_LEN    32
#define SHORT_NONCE_LEN  12
#define LONG_NONCE_LEN   16
#define HMAC_TRUNC_BYTES 8

#define SIGNATURE_LEN  ((2 * sizeof(bigint)) + (2 * PRIVKEY_LEN))

volatile u8 temp_handshake_memory_region_isLocked = 0;

struct roommate{
    char   user_id[SMALL_FIELD_LEN];
    u64    client_nonce_counter;
    bigint client_pubkey;
    bigint client_pubkey_mont;
    bigint client_shared_secret; 
};

u64  own_ix = 0;
char own_user_id[SMALL_FIELD_LEN];

bigint server_shared_secret;
u64    server_nonce_counter;

pthread_mutex_t mutex;
pthread_t poller_threadID;

u8 own_privkey_buf[PRIVKEY_LEN];

struct bigint *M, *Q, *G, *Gm, *server_pubkey_bigint, *own_privkey, *own_pubkey
             ,*server_pubkey_mont;

/* Memory region holding short-term cryptographic artifacts for Login scheme. */
u8* temp_handshake_buf;

#define PACKET_ID_00 0xAD0084FF0CC25B0E
#define PACKET_ID_01 0xE7D09F1FEFEA708B
#define PACKET_ID_02 0x146AAE4D100DAEEA
#define PACKET_ID_10 0x13C4A44F70842AC1
#define PACKET_ID_11 0xAEFB70A4A8E610DF
#define PACKET_ID_20 0x9FF4D1E0EAE100A5
#define PACKET_ID_21 0x7C8124568ED45F1A
#define PACKET_ID_30 0x9FFA7475DDC8B11C
#define PACKET_ID_40 0xCAFB1C01456DF7F0
#define PACKET_ID_41 0xDC4F771C0B22FDAB
#define PACKET_ID_50 0x41C20F0BB4E34890
#define PACKET_ID_51 0x2CC04FBEDA0B5E63
#define PACKET_ID_60 0x0A7F4E5D330A14DD

/* Linux Sockets API related globals. */
int port = SERVER_PORT
   ,own_socket_fd
   ,optval1 = 1
   ,optval2 = 2;

socklen_t server_addr_len = sizeof(struct sockaddr_in);

struct sockaddr_in server_address;


/* First thing done when we start the client software - initialize it. */
u32 self_init(){

    /* Allocate memory for the temporary login handshake memory region. */
    temp_handshake_buf = calloc(1, TEMP_BUF_SIZ);
 
    /* Initialize our own socket. */
    own_socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    
    if(own_socket_fd == -1) {
        printf("[ERR] Client: Own TCP socket init failed. Terminating.\n");
        return 1;
    }

    setsockopt(
          own_socket_fd, SOL_SOCKET, SO_REUSEPORT, &optval1, sizeof(optval1)
    );  
      
    setsockopt(
          own_socket_fd, SOL_SOCKET, SO_REUSEADDR, &optval2, sizeof(optval2)
    );
    
    explicit_bzero(&server_address, sizeof(struct sockaddr_in));
 
    /* Initialize the server address structure. */
    servaddr.sin_family      = AF_INET;
    servaddr.sin_addr.s_addr = inet_addr("212.104.116.132");
    servaddr.sin_port        = htons(PORT);
 
    /* Load our own private key. */
    FILE* privkey_dat = fopen("client_privkey.dat", "r"); 
    
    if(!privkey_dat){
        printf("[ERR] Client: couldn't open private key DAT file. Aborting.\n");
        return 1;
    }       
    
    if(fread(own_privkey_buf, 1, PRIVKEY_LEN, privkey_dat) != PRIVKEY_LEN){
        printf("[ERR] Client: couldn't get private key from file. Aborting.\n");
        fclose(privkey_dat);
        return 1;
    }

    fclose(privkey_dat);

    /* Initialize the BigInt that stores our private key. */
    bigint_create(&own_privkey, MAX_BIGINT_SIZ, 0);
    memcpy(own_privkey.bits, own_privkey_buf, PRIVKEY_LEN);     
    own_privkey.used_bits = get_used_bits(own_privkey_buf, PRIVKEY_LEN);                            
    own_privkey.free_bits = MAX_BIGINT_SIZ - own_privkey.used_bits;   

    /* Load in other BigInts needed for the cryptography to work. */
    
    /* Diffie-Hellman modulus M, 3071-bit prime number */                        
    M = get_BIGINT_from_DAT
        (3072, "../../saved_nums/M_raw_bytes.dat\0", 3071, MAX_BIGINT_SIZ);
    
    /* 320-bit prime exactly dividing M-1, making M cryptographycally strong. */
    Q = get_BIGINT_from_DAT
        (320,  "../../saved_nums/Q_raw_bytes.dat\0", 320,  MAX_BIGINT_SIZ);
    
    /* Diffie-Hellman generator G = G = 2^((M-1)/Q) */
    G = get_BIGINT_from_DAT
        (3072, "../../saved_nums/G_raw_bytes.dat\0", 3071, MAX_BIGINT_SIZ);

    /* Montgomery Form of G, since we use Montgomery Modular Multiplication. */
    Gm = get_BIGINT_from_DAT( 3072
                             ,"../../saved_nums/PRACTICAL_Gmont_raw_bytes.dat\0"
                             ,3071
                             ,MAX_BIGINT_SIZ
    );
    
    server_pubkey_bigint = get_BIGINT_from_DAT
        (3072, "../../saved_nums/server_pubkey.dat\0", 3071, MAX_BIGINT_SIZ);
           
    bigint_create(server_pubkey_mont, MAX_BIGINT_SIZ, 0);  
           
    Get_Mont_Form(server_pubkey_bigint, server_pubkey_mont, M);
    
    /* Initialize the shared secret with the server. */
    MONT_POW_modM(server_pubkey_mont, own_privkey, M, &server_shared_secret);
    
    own_pubkey = get_BIGINT_from_DAT
        (3072, "../../saved_nums/own_pubkey.dat\0", 3071, MAX_BIGINT_SIZ);
    
    /* Initialize the mutex that will be used to prevent the main thread and
     * the poller thread from writing/reading the same data in parallel.
     */
    if (pthread_mutex_init(&mutex, NULL) != 0) { 
        printf("[ERR] Server: Mutex could not be initialized. Aborting.\n"); 
        return 1; 
    } 
    
    /* Initialize the nonce increment count with the server to 0. */    
    server_nonce_counter = 0;
    
    return 0;
}

u8 authenticate_server(u8* signed_ptr, u64 signed_len, u64 signature_offset){

    bigint *recv_e;
    bigint *recv_s;
    
    u64 s_offset = sign_offset;
    u64 e_offset = (sign_offset + sizeof(bigint) + PRIVKEY_LEN);
   
    u8 ret;
    
    /* Reconstruct the sender's signature as the two BigInts that make it up. */
    recv_s = (bigint*)(signed_ptr + s_offset);
    recv_e = (bigint*)(signed_ptr + e_offset);    
    
    recv_s->bits = calloc(1, MAX_BIGINT_SIZ);
    recv_e->bits = calloc(1, MAX_BIGINT_SIZ);
 
    memcpy( recv_s->bits
           ,signed_ptr + (sign_offset + sizeof(bigint))
           ,PRIVKEY_LEN
    );
    
    memcpy( recv_e->bits
           ,signed_ptr + (sign_offset + (2*sizeof(bigint)) + PRIVKEY_LEN)
           ,PRIVKEY_LEN
    );
       
    /* Verify the sender's cryptographic signature. */
    ret = Signature_VALIDATE(
            Gm, server_pubkey_mont, M, Q, recv_s, recv_e, signed_ptr, signed_len
    ); 

    free(recv_s->bits);
    free(recv_e->bits);

    return ret;
}


/* A user requested to be logged in Rosetta:

    Client ----> Server

================================================================================
|        PACKET_ID_00         |   Client's short-term public key in the clear  |
|=============================|================================================|
|       SMALL_FIELD_LEN       |                    PUBKEY_LEN                  |
--------------------------------------------------------------------------------

*/
u8 construct_msg_00(void){

    bigint* a_s;
    bigint* A_s;
    
    u8 status = 0;
    
    const u64 msg_len = SMALL_FIELD_LEN + PUBKEY_LEN;
    u8* msg_buf = calloc(1, msg_len);

    /* Generate client's short-term public/private key pair and ChaCha nonce N, 
     * shared secret and thus bidirectional keys KAB, KBA and "unused" part Y:
     *
     *       a_s = random in the range [1, Q)
     * 
     *       A_s = G^b_s mod M     <--- Montgomery Form of G.
     *   
     *       B_s = Server's one-time public key for login handshake.
     *
     *       X_s = B_s^a_s mod M   <--- Shared secret. Montgomery Form of B_s.
     *
     *       KAB_s = X_s[0  .. 31 ]
     *       KBA_s = X_s[32 .. 63 ]
     *       Y_s   = X_s[64 .. 95 ] 
     *       N_s   = X_s[96 .. 107] <--- 12-byte Nonce for ChaCha20.    
     */
     
    temp_handshake_memory_region_isLocked = 1;
    
    gen_priv_key(PRIVKEY_LEN, temp_handshake_buf);
    
    a_s = (bigint*)(temp_handshake_buf);

    /* Interface generating a pub_key still needs priv_key in a file. Change. */
    save_BIGINT_to_DAT("temp_privkey_DAT\0", a_s);
  
    A_s = gen_pub_key(PRIVKEY_LEN, "temp_privkey_DAT\0", MAX_BIGINT_SIZ);
    
    /* Place our short-term pub_key also in the locked memory region. */
    memcpy(temp_handshake_buf + sizeof(bigint), A_s, sizeof(bigint));

    /* Establish an initial connection to the Rosetta TCP server. */
    printf("[OK]  Client: Now connecting to the Rosetta TCP server...\n");
    
    if( connect(own_socket_fd, (struct sockaddr*)&servaddr, sizeof(servaddr)) ){
        printf("[ERR] Client: Couldn't connect to the Rosetta TCP server.\n");
        printf("              Aborting Login...\n\n");
        status = 1;
        goto label_cleanup;
    }
    else{
        printf("[OK]  Client: Connected to the Rosetta TCP server!\n");
    }
    
    /* Construct and send the MSG buffer to the TCP server. */
    
    *((u64*)(msg_buf)) = (u64)PACKET_ID_00;
    
    memcpy(msg_buf + SMALL_FIELD_LEN, A_s->bits, PUBKEY_LEN);
    
    if(send(own_socket_fd, msg_buf, msg_len) == -1){
        printf("[ERR] Client: Couldn't send initial login transmission.\n");
        printf("[ERR]         Aborting Login...\n\n");
        status = 1;
        goto label_cleanup;
    }
    else{
        printf("[OK]  Client: Sent initial login transmission!\n");
    }
    
label_cleanup:

    system("rm temp_privkey_DAT");
    free(msg_buf);
    
    return status;
}

/* Server sent its short-term public key too, so the client can now compute a
   shared secret and transport its LONG-TERM public key in encrypted form and
   obtain its user index, completing the login handshake.
   
    Server ----> Client

================================================================================
| PACKET_ID_00 | Server's one time PubKey | Signature of unused part of X: Y_s |
|==============|==========================|====================================|
|  SMALL_LEN   |       PUBKEY_LEN         |             SIGNATURE_LEN          |
--------------------------------------------------------------------------------

*/  
void process_msg_00(u8* msg_buf){

    u64 handshake_buf_key_offset;
    u64 handshake_buf_nonce_offset;
    
    u32 tempbuf_write_offset;
    u32 shared_secret_read_offset;
    u32 HMAC_reply_offset = SMALL_FIELD_LEN + PUBKEY_LEN;
    bigint *X_s;
    bigint *B_s;
    bigint  B_sM;
    bigint  zero;
    bigint *a_s = (bigint*)(temp_handshake_buf);
    
    u8  auth_status;
    u8* auth_buf = calloc(1, (INIT_AUTH_LEN + SIGNATURE_LEN));
        
    u64 B = 64;
    u64 L = 128;
    
    u8* K0   = calloc(1, B);
    u8* ipad = calloc(1, B);
    u8* opad = calloc(1, B);
    u8* K0_XOR_ipad_TEXT = calloc(1, (B + PUBKEY_LEN));
    u8* BLAKE2B_output = calloc(1, L);   
    u8* last_BLAKE2B_input = calloc(1, (B + L));
    u8* K0_XOR_ipad = calloc(1, B);
    u8* K0_XOR_opad = calloc(1, B);
    
    u64 reply_len = SMALL_FIELD_LEN + PUBKEY_LEN + HMAC_TRUNC_BYTES;
    u8* reply_buf = calloc(1, reply_len);

    /* Grab the server's short-term public key from the transmission. */
    B_s = calloc(1, sizeof(bigint));
    B_s->bits = calloc(1, MAX_BIGINT_SIZ);
    
    memcpy(B_s->bits, msg_buf + SMALL_FIELD_LEN, PUBKEY_LEN);
    
    B_s->size_bits = MAX_BIGINT_SIZ;
    B_s->used_bits = get_used_bits(B_s->bits, PUBKEY_LEN);
    B_s->free_bits = B_s->size_bits - B_s->used_bits;
    
    /* Compute a short-term shared secret with the server, extract a pair of
     * symmetric bidirectional keys and the symmetric ChaCha Nonce, as well as
     * the unused part of the shared secret, of which the server has computed
     * a cryptographic signature, which we need to verify for authentication.
     *
     *       X_s   = B_s^a_s mod M   <--- Montgomery Form of B_s.
     *
     *       KAB_s = X_s[0  .. 31 ]
     *       KBA_s = X_s[32 .. 63 ]
     *       Y_s   = X_s[64 .. 95 ]
     *       N_s   = X_s[96 .. 107]  <--- 12-byte Nonce for ChaCha20.
     */
    
    bigint_create(X_s,   MAX_BIGINT_SIZ, 0);
    bigint_create(&zero, MAX_BIGINT_SIZ, 0);
    bigint_create(&B_sM, MAX_BIGINT_SIZ, 0);
    
    Get_Mont_Form(B_s, &B_sM, M);
    
    /* Check the other side's public key for security flaws and consistency. */   
    if(   ((bigint_compare2(&zero, B_s)) != 3) 
        || 
          ((bigint_compare2(M, B_s)) != 1)
        ||
          (check_pubkey_form(&A_sM, M, Q) == 0) 
      )
    {
        printf("[ERR] Client: Server's short-term public key is invalid.\n");
        printf("              Its info and ALL %u bits:\n\n");
        bigint_print_info(B_s);
        bigint_print_all_bits(B_s);
        goto label_cleanup;
    }     
        
    /* X_s = B_s^a_s mod M */
    MONT_POW_modM(&B_sm, a_s, M, X_s);
    
    /* Construct a special buffer containing Y_s concatenated with the received
     * signature, because the signature validating interface needs it that way
     * because that's how most use cases of it have their buffers structured -
     * the signature to be validated is in the same memory buffer as what was
     * signed to begin with.
     */
    memcpy( auth_buf
           ,X_s->bits + (2 * SESSION_KEY_LEN)
           ,INIT_AUTH_LEN
          );
    
    memcpy( auth_buf + INIT_AUTH_LEN
           ,msg_buf  + (SMALL_FIELD_LEN + PUBKEY_LEN)
           ,SIGNATURE_LEN
          );
    
    /* Validate the signature of the unused part of the shared secret, Y_s. */
    auth_status = authenticate_server(auth_buf, INIT_AUTH_LEN, INIT_AUTH_LEN);
    
    if(auth_status != 1){
        printf("[ERR] Client: Invalid signature in process_msg_00. Drop.\n\n");
        goto label_cleanup;           
    }
    
    /* Transport the 2 symmetric keys, server's one-time public key and the 
     * 2 cryptographic artifacts (N, Y) to the designated locked memory region
     */
     
    tempbuf_write_offset      = 2 * sizeof(bigint);
    shared_secret_read_offset = 0;
    
    /* Key B_s */
    memcpy(temp_handshake_buf + tempbuf_write_offset, &B_s, sizeof(bigint*));
    
    tempbuf_write_offset += sizeof(bigint*);
    
    /* Key KAB_s */
    memcpy( temp_handshake_buf + tempbuf_write_offset
           ,X_s->bits
           ,SESSION_KEY_LEN
          );
    
    shared_secret_read_offset += SESSION_KEY_LEN;
    tempbuf_write_offset      += SESSION_KEY_LEN;
    
    /* Key KBA_s */
    memcpy( temp_handshake_buf + tempbuf_write_offset
           ,X_s->bits + shared_secret_read_offset
           ,SESSION_KEY_LEN
          );
          
    shared_secret_read_offset += SESSION_KEY_LEN;
    tempbuf_write_offset      += SESSION_KEY_LEN;      
    
    /* Section of shared secret on which we'll compute Schnorr signatures. */
    memcpy( temp_handshake_buf + tempbuf_write_offset
        ,X_s->bits + shared_secret_read_offset
        ,INIT_AUTH_LEN
       );

    shared_secret_read_offset += INIT_AUTH_LEN;
    tempbuf_write_offset      += INIT_AUTH_LEN;  
    
    /* short-term symmetric Nonce for encrypting/decrypting with ChaCha20 */
    memcpy( temp_handshake_buf + tempbuf_write_offset
        ,X_s->bits + shared_secret_read_offset
        ,SHORT_NONCE_LEN
       );
     
    /* Ready to start constructing the reply buffer to the server. */ 
        
    *((u64*)(reply_buf)) = PACKET_ID_01;
       
    /*  Client uses KAB_s as key and 12-byte N_s as Nonce in ChaCha20 to
     *  encrypt its long-term public key A, producing the key A_x.
     *
     *  Sends that encrypted long-term public key to the Rosetta server.
     */
    handshake_buf_nonce_offset =  
                (3 * sizeof(bigint)) + (2 * SESSION_KEY_LEN) + INIT_AUTH_LEN;
                                                            
    handshake_buf_key_offset = 3 * sizeof(bigint);
    
    /* Passed parameters to this call to ChaCha20:
     *
     *  1. INPUT TEXT   : Client's long-term public key unencrypted
     *  2. TEXT_length  : in bytes
     *  3. ChaCha Nonce : inside the locked global handshake memory region.
     *  4. Nonce_length : in uint32_t's
     *  5. ChaCha Key   : inside the locked global handshake memory region
     *  6. Key_length   : in uint32_t's.
     *  7. Destination  : Pointer to correct offset into the reply buffer.
     */
    CHACHA20( own_pubkey->bits
             ,PUBKEY_LEN
             ,(u32*)(temp_handshake_buf + handshake_buf_nonce_offset)
             ,(u32)(SHORT_NONCE_LEN / sizeof(u32))       
             ,(u32*)(temp_handshake_buf + handshake_buf_key_offset)
             ,(u32)(SESSION_KEY_LEN / sizeof(u32))
             ,reply_buf + SMALL_FIELD_LEN
            );
            
    /* Increment the Nonce to not reuse it when decrypting our user index. */       
    ++(*(temp_handshake_buf + handshake_buf_nonce_offset));
       
    /* Only thing left to construct is the HMAC authenticator now. */ 
    memset(opad, 0x5c, B);
    memset(ipad, 0x36, B);
    
    /*  Use what's already in the locked memory region to compute HMAC and 
     *  to decrypt the user's long-term public key
     *
     *  Server uses KAB_s to compute the same HMAC on A_x (client's long-term
     *  public key in encrypted form) as the client did. 
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
       
    /* Step 3 of HMAC construction: HMAC key (KAB) 0-extended to B bytes. */
    memcpy( K0 + (B - SESSION_KEY_LEN)
           ,temp_handshake_buf + (3 * sizeof(bigint))
           ,SESSION_KEY_LEN
          ); 
    
    /* Step 4 of HMAC construction */
    for(u64 i = 0; i < B; ++i){
        K0_XOR_ipad[i] = (K0[i] ^ ipad[i]);
    }       
       
    /* step 5 of HMAC construction */
    memcpy(K0_XOR_ipad_TEXT, K0_XOR_ipad, B);
    memcpy(K0_XOR_ipad_TEXT + B, reply_buf + SMALL_FIELD_LEN, PUBKEY_LEN);
    
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
    memcpy(last_BLAKE2B_input + 0, K0_XOR_opad,    B);
    memcpy(last_BLAKE2B_input + B, BLAKE2B_output, L);    
    
    /* Step 9 of HMAC construction */ 
    /* Call BLAKE2B on the combined buffer in step 8. */
    BLAKE2B_INIT(last_BLAKE2B_input, B + L, 0, L, BLAKE2B_output);
    
    /* Take the HMAC_TRUNC_BYTES leftmost bytes to form the HMAC output. */
    memcpy(reply_buf + HMAC_reply_offset, BLAKE2B_output, HMAC_TRUNC_BYTES);

/*  Now send the reply back to the Rosetta server:

================================================================================
|  packet ID 01   | Client's encrypted long-term PubKey |  HMAC authenticator  |
|=================|=====================================|======================|
| SMALL_FIELD_LEN |             PUBKEY_LEN              |   HMAC_TRUNC_BYTES   |
--------------------------------------------------------------------------------

*/
    if(send(own_socket_fd, reply_buf, reply_len) == -1){
        printf("[ERR] Client: Couldn't send second login transmission.\n");
        printf("[ERR]         Aborting Login...\n\n");
        status = 1;
        goto label_cleanup;
    }
    else{
        printf("[OK]  Client: Sent second login transmission!\n");
    }
    
label_cleanup:
   
    free(K0);
    free(ipad);
    free(opad);
    free(K0_XOR_ipad_TEXT);
    free(BLAKE2B_output);   
    free(last_BLAKE2B_input);
    free(K0_XOR_ipad);
    free(K0_XOR_opad);   
    free(reply_buf);
    free(auth_buf);
    
    return;
}

/* This function is one of two possible ones to be called after the listen() 
 * in the main processor blocks, expecting an answer after our 2nd login packet.
 *
 * This one is when the Login handshake was successful, there was room in 
 * Rosetta for the client, and the server has sent us our user index.
 *
 * Authenticate it, process its contents and alert the user's GUI that login 
 * went OK, so it can show it to the user and show the buttons to join or create
 * a chatroom.
 */

/*  
 
    Server ----> Client
  
================================================================================
| packet ID 01 |  user_ix  |                    SIGNATURE                      | 
|==============|===========|===================================================|
|  SMALL_LEN   | SMALL_LEN |                     SIG_LEN                       |
--------------------------------------------------------------------------------

*/

u8 process_msg_01(u8* msg){

    u64 nonce_offset;
    U64 key_offset;
    
    u8 status = 0;

    /* Validate the incoming signature with the server's long-term public key
     * on packet_ID_01 (for now... later it will be of the whole payload).
     */    
    
    status = authenticate_server(msg, SMALL_FIELD_LEN, (2 * SMALL_FIELD_LEN));

    if(status != 1){
        printf("[ERR] Client: Invalid signature in process_msg_01. Drop.\n\n");
        status = 0;
        goto label_cleanup;           
    }
    
    /* Signature is valid! Can locate our index, decrypt it and save it. */
    
    nonce_offset = (2 * sizeof(bigint))  + sizeof(bigint*) + 
                   (2 * SESSION_KEY_LEN) + INIT_AUTH_LEN; 
    
    key_offset = (2 * sizeof(bigint)) + sizeof(bigint*) + SESSION_KEY_LEN ;  
                 
    CHACHA20( msg_buf + SMALL_FIELD_LEN                /* text - key KB       */
             ,SMALL_FIELD_LEN                          /* text_len in bytes   */
             ,(u32*)(temp_handshake_buf + nonce_offset)/* Nonce ptr           */
             ,(u32)(SHORT_NONCE_LEN / sizeof(u32))     /* nonceLen in uint32s */
             ,(u32*)(temp_handshake_buf + key_offset)  /* chacha Key ptr      */
             ,(u32)(SESSION_KEY_LEN / sizeof(u32))     /* Key_len in uint32s  */
             ,&own_ix                                  /* output buffer ptr   */
            );
      
label_cleanup:             
             
    explicit_bzero(temp_handshake_buf, TEMP_BUF_SIZ);
    
    temp_handshake_memory_region_isLocked = 0;     
    
    return status; 
     
}

/* This function is one of two possible ones to be called after the listen() 
 * in the main processor blocks, expecting an answer after our 2nd login packet.
 *
 * This one is when the server told us Rosetta is full. Verify the signature to
 * make sure the reply was really sent by the Rosetta server and that it wasn't
 * modified by a man in the middle attack somewhere along the way. 
 *
 * If it's valid, tell the user's GUI that there is no room in Rosetta right now 
 * and to try logging in later, so it can display that to the user.
 */
 
/* 
    Server ----> Client
  
================================================================================
| packet ID 02 |                         SIGNATURE                             | 
|==============|===============================================================|
|  SMALL_LEN   |                          SIG_LEN                              |
--------------------------------------------------------------------------------

*/
u8 process_msg_02(u8* msg){

    u8 status;

    /* Validate the incoming signature with the server's long-term public key
     * on packet_ID_02.
     */    
    
    status = authenticate_server(msg, SMALL_FIELD_LEN, SMALL_FIELD_LEN);    

    if(status != 1){
        printf("[ERR] Client: Invalid signature in process_msg_02. Drop.\n\n");
        status = 0;       
    }

    return status;
}

/* The user has requested to create a new chatroom.
 
                                          ENCRYPTED
                            /\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\
================================================================================
| packet ID 10 |  user_ix  | Decryption Key   | Room_ID+user_ID |  Signature   |
|==============|===========|==================|=================|==============|
|  SMALL_LEN   | SMALL_LEN | ONE_TIME_KEY_LEN |  2 * SMALL_LEN  | SIGNATURE_LEN|
--------------------------------------------------------------------------------

*/
u8 construct_msg_10( unsigned char* requested_userid
                    ,unsigned char* requested_roomid )
{



}































