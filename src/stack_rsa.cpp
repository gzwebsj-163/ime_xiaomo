#include "./include/stack_rsa.h"
#include <string>
#include <cstring>
using std::vector;
std::string generate_random_uuid() {
    uint8_t uuid_bytes[16] = {0};
    if (RAND_bytes(uuid_bytes, sizeof(uuid_bytes)) != 1) {
        fprintf(stderr, "生成 UUID 随机数失败：");
        ERR_print_errors_fp(stderr);
        return "";
    }
    uuid_bytes[6] = (uuid_bytes[6] & 0x0F) | 0x40;
    uuid_bytes[8] = (uuid_bytes[8] & 0x3F) | 0x80;
    char uuid_str[37] = {0};
    snprintf(uuid_str, sizeof(uuid_str),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             uuid_bytes[0],  uuid_bytes[1],  uuid_bytes[2],  uuid_bytes[3],
             uuid_bytes[4],  uuid_bytes[5],  uuid_bytes[6],  uuid_bytes[7],
             uuid_bytes[8],  uuid_bytes[9],  uuid_bytes[10], uuid_bytes[11],
             uuid_bytes[12], uuid_bytes[13], uuid_bytes[14], uuid_bytes[15]);
    return std::string(uuid_str);
}



bool rsa_verify_key(RSA* rsa) {
   if (rsa == nullptr) {
        fprintf(stderr, "RSA 对象为空\n");
        return false;
    }
    int bits = RSA_size(rsa) * 8;
    if (bits != 2048 && bits != 4096) {
        fprintf(stderr, "RSA 密钥长度非法：%d 位（预期 2048/4096 位）\n", bits);
        return false;
    }
    const BIGNUM* e = RSA_get0_e(rsa);
    if (e == nullptr || !BN_is_word(e, RSA_F4)) {
        fprintf(stderr, "RSA 公钥指数非法\n");
        return false;
    }
    std::string uuid_str = generate_random_uuid();
    if (uuid_str.empty()) {
        fprintf(stderr, "生成随机 UUID 失败，无法验证密钥\n");
        return false;
    }
    const char* uuid = uuid_str.c_str();
    int data_len = uuid_str.length();
    fprintf(stdout, "\n生成随机测试 UUID：%s\n", uuid);
    int rsa_size = RSA_size(rsa);
    unsigned char* encrypt_buf = (unsigned char*)malloc(rsa_size);
    unsigned char* decrypt_buf = (unsigned char*)malloc(rsa_size);
    if (encrypt_buf == nullptr || decrypt_buf == nullptr) {
        fprintf(stderr, "内存分配失败\n");
        free(encrypt_buf);
        free(decrypt_buf);
        return false;
    }
    int encrypt_len = RSA_public_encrypt(
        data_len,
        (unsigned char*)uuid,
        encrypt_buf,
        rsa,
        RSA_PKCS1_PADDING
    );
    if (encrypt_len < 0) {
        fprintf(stderr, "公钥加密失败：");
        ERR_print_errors_fp(stderr);
        free(encrypt_buf);
        free(decrypt_buf);
        return false;
    }
    int decrypt_len = RSA_private_decrypt(
        encrypt_len,
        encrypt_buf,
        decrypt_buf,
        rsa,
        RSA_PKCS1_PADDING
    );
    if (decrypt_len < 0) {
        fprintf(stderr, "私钥解密失败：");
        ERR_print_errors_fp(stderr);
        free(encrypt_buf);
        free(decrypt_buf);
        return false;
    }

    bool decrypt_ok = false;
    if (decrypt_len == data_len && memcmp(decrypt_buf, uuid, data_len) == 0) {
        decrypt_ok = true;
        fprintf(stdout, "加密解密验证通过：解密结果 = %.*s\n", decrypt_len, decrypt_buf);
    } else {
        fprintf(stderr, "加密解密验证失败：\n");
        fprintf(stderr, "   原 UUID：%s\n", uuid);
        if (decrypt_len > 0) {
            fprintf(stderr, "   解密结果：%.*s\n", decrypt_len, decrypt_buf);
        } else {
            fprintf(stderr, "   解密结果：无\n");
        }
    }

    bool sign_verify_ok = false;
    unsigned char sign_buf[rsa_size];
    unsigned int sign_len = 0;

    if (RSA_sign(
            NID_sha256,
            (unsigned char*)uuid,
            data_len,
            sign_buf,
            &sign_len,
            rsa
        ) == 1) {
        if (RSA_verify(
                NID_sha256,
                (unsigned char*)uuid,
                data_len,
                sign_buf,
                sign_len,
                rsa
            ) == 1) {
            sign_verify_ok = true;
            fprintf(stdout, "签名验签验证通过\n");
        } else {
            fprintf(stderr, "签名验签失败：");
            ERR_print_errors_fp(stderr);
        }
    } else {
        fprintf(stderr, "私钥签名失败：");
        ERR_print_errors_fp(stderr);
    }
    free(encrypt_buf);
    free(decrypt_buf);
    return decrypt_ok && sign_verify_ok;
}


std::string rsa_to_pem(RSA* rsa, bool is_public) {
    if (rsa == nullptr) {
        return "";
    }
    BIO* bio = BIO_new(BIO_s_mem());
    if (bio == nullptr) {
        ERR_print_errors_fp(stderr);
        return "";
    }
    int ret = 0;
    if (is_public) {
        ret = PEM_write_bio_RSAPublicKey(bio, rsa);
    } else {
        ret = PEM_write_bio_RSAPrivateKey(bio, rsa, nullptr, nullptr, 0, nullptr, nullptr);
    }

    if (ret != 1) {
        ERR_print_errors_fp(stderr);
        BIO_free(bio);
        return "";
    }
    char* pem_data = nullptr;
    long len = BIO_get_mem_data(bio, &pem_data);
    std::string pem_str(pem_data, len);
    BIO_free(bio);
    return pem_str;
}

bool openssl_setup(OPENSSL_CALLBACK call, std::vector<RSA *> &name, std::vector<RSA *> &key, std::vector<RSA *> &value, std::vector<RSA *> &debug)
{

       if (call == nullptr)
    {
        fprintf(stdout, "openssl 指针不存在或空指针\n");
        return false;
    }
    #if OPENSSL_VERSION_NUMBER >= 0x30000000L
    OPENSSL_init_crypto(OPENSSL_INIT_LOAD_CRYPTO_STRINGS | OPENSSL_INIT_ADD_ALL_CIPHERS | OPENSSL_INIT_ADD_ALL_DIGESTS, nullptr);
    #else
    OpenSSL_add_all_algorithms();
    ERR_load_crypto_strings();
    #endif
    call("openssl 初始化", nullptr);
    RSA *obj = RSA_new();
    if (obj == nullptr)
    {
        call("创建RSA失败", nullptr);
        ERR_print_errors_fp(stderr);
        return false;
    }
    BIGNUM *big = BN_new();
    if (big == nullptr || !BN_set_word(big, RSA_F4))
    {
        call("初始化bignum失败", nullptr);
        RSA_free(obj);
        BN_free(big);
        ERR_print_errors_fp(stderr);
        return false;
    }
    if (!RSA_generate_key_ex(obj, 2048, big, nullptr))
    {
        call("创建密钥失败", nullptr);
        RSA_free(obj);
        BN_free(big);
        ERR_print_errors_fp(stderr);
        return false;
    }
    std::string public_key = rsa_to_pem(obj, true);
    std::string private_key = rsa_to_pem(obj, false);
    fprintf(stdout, "\n===== RSA 公钥（PEM 格式）=====\n%s\n", public_key.c_str());
    fprintf(stdout, "===== RSA 私钥（PEM 格式）=====\n%s\n", private_key.c_str());
    call("RSA 公钥", const_cast<void*>(static_cast<const void*>(public_key.c_str())));
    call("RSA 私钥", const_cast<void*>(static_cast<const void*>(private_key.c_str())));
    fprintf(stdout, "\n开始验证 RSA 密钥有效性...\n");
    if (!rsa_verify_key(obj)) {
        call("RSA 密钥无效", nullptr);
        RSA_free(obj);
        BN_free(big);
        return false;
    }
    call("RSA 密钥验证通过，密钥有效！", nullptr);
    name.push_back(obj);
    call("OpenSSL 环境初始化成功，RSA 密钥对生成并验证完成！", nullptr);
    BN_free(big);
    return true;
}