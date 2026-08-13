#ifndef STACK_RSA_H
#define STACK_RSA_H
#include <openssl/rsa.h>
#include <openssl/evp.h>
#include <openssl/err.h>
#include <openssl/crypto.h>
#include <openssl/bn.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <stdio.h>
#include <cstring>
#include <string>
#include <vector>
typedef int (*OPENSSL_CALLBACK)(const char* msg, void* user_data);
#define IS_CHECK_OPENSSL(_openssl, openssl) \
    (((_openssl) == nullptr) || ((openssl) == 0) || (stack_is_full(_openssl) != 0))
    bool openssl_setup(OPENSSL_CALLBACK call, std::vector<RSA *> &name, std::vector<RSA *> &key, std::vector<RSA *> &value, std::vector<RSA *> &debug);
    std::string generate_random_uuid();
    bool rsa_key_verify(RSA* rsa);
    std::string rsa_to_pem(RSA* rsa, bool is_public);
    bool rsa_encrypt_decrypt(RSA* rsa, const std::string& uuid, std::string& out_decrypt);
#endif
