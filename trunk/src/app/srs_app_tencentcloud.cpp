//
// Copyright (c) 2013-2022 The SRS Authors
//
// SPDX-License-Identifier: MIT or MulanPSL-2.0
//

#include <srs_app_tencentcloud.hpp>

#include <srs_kernel_error.hpp>
#include <srs_kernel_buffer.hpp>
#include <srs_app_config.hpp>
#include <srs_protocol_http_client.hpp>
#include <srs_protocol_http_stack.hpp>
#include <srs_kernel_utility.hpp>
#include <srs_protocol_utility.hpp>
#include <srs_core_autofree.hpp>
#include <srs_protocol_json.hpp>
#include <srs_app_statistic.hpp>
#include <srs_kernel_utility.hpp>
#include <srs_app_utility.hpp>

#include <string>
#include <map>
#include <algorithm>
#include <stdio.h>
#include <string.h>
#include <openssl/sha.h>
#include <openssl/hmac.h>
#include <unistd.h>
using namespace std;

// See https://cloud.tencent.com/document/product/614/12445
namespace tencentcloud_api_sign {
    std::string sha1(const void *data, size_t len) {
        unsigned char digest[SHA_DIGEST_LENGTH];
        SHA_CTX ctx;
        SHA1_Init(&ctx);
        SHA1_Update(&ctx, data, len);
        SHA1_Final(digest, &ctx);
        char c_sha1[SHA_DIGEST_LENGTH*2+1];
        for (unsigned i = 0; i < SHA_DIGEST_LENGTH; ++i) {
            sprintf(&c_sha1[i*2], "%02x", (unsigned int)digest[i]);
        }
        return c_sha1;
    }

    std::string hmac_sha1(const char *key, const void *data, size_t len) {
        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned digest_len;
        char c_hmacsha1[EVP_MAX_MD_SIZE*2+1];
#if !defined(OPENSSL_VERSION_NUMBER) || OPENSSL_VERSION_NUMBER < 0x10100000L
        HMAC_CTX ctx;
        HMAC_CTX_init(&ctx);
        HMAC_Init_ex(&ctx, key, strlen(key), EVP_sha1(), NULL);
        HMAC_Update(&ctx, (unsigned char*)data, len);
        HMAC_Final(&ctx, digest, &digest_len);
        HMAC_CTX_cleanup(&ctx);
#else
        HMAC_CTX *ctx = HMAC_CTX_new();
        HMAC_CTX_reset(ctx);
        HMAC_Init_ex(ctx, key, strlen(key), EVP_sha1(), NULL);
        HMAC_Update(ctx, (unsigned char *)data, len);
        HMAC_Final(ctx, digest, &digest_len);
        HMAC_CTX_free(ctx);
#endif
        for (unsigned i = 0; i != digest_len; ++i) {
            sprintf(&c_hmacsha1[i*2], "%02x", (unsigned int)digest[i]);
        }
        return c_hmacsha1;
    }

    std::string urlencode(const char *s) {
        static unsigned char hexchars[] = "0123456789ABCDEF";
        size_t length = strlen(s), pos = 0;
        unsigned char c_url[length*3+1];
        const unsigned char *p = (const unsigned char *)s;
        for (; *p; ++p) {
            if (isalnum((unsigned char)*p) || (*p == '-') ||
                (*p == '_') || (*p == '.') || (*p == '~')) {
                c_url[pos++] = *p;
            } else {
                c_url[pos++] = '%';
                c_url[pos++] = hexchars[(*p)>>4];
                c_url[pos++] = hexchars[(*p)&15U];
            }
        }
        c_url[pos] = 0;
        return (char*)c_url;
    }

    std::string signature(const std::string &secret_id,
                          const std::string &secret_key,
                          std::string method,
                          const std::string &path,
                          const std::map<std::string, std::string> &params,
                          const std::map<std::string, std::string> &headers,
                          long expire) {

        const size_t SIGNLEN = 1024;
        std::string http_request_info, uri_parm_list,
                header_list, str_to_sign, sign_key;
        transform(method.begin(), method.end(), method.begin(), ::tolower);
        http_request_info.reserve(SIGNLEN);
        http_request_info.append(method).append("\n").append(path).append("\n");
        uri_parm_list.reserve(SIGNLEN);
        std::map<std::string, std::string>::const_iterator iter;
        for (iter = params.begin();
             iter != params.end(); ) {
            uri_parm_list.append(iter->first);
            http_request_info.append(iter->first).append("=")
                    .append(urlencode(iter->second.c_str()));
            if (++iter != params.end()) {
                uri_parm_list.append(";");
                http_request_info.append("&");
            }
        }
        http_request_info.append("\n");
        header_list.reserve(SIGNLEN);
        for (iter = headers.begin();
             iter != headers.end(); ++iter) {
            sign_key = iter->first;
            transform(sign_key.begin(), sign_key.end(), sign_key.begin(), ::tolower);
            if (sign_key == "content-type" || sign_key == "content-md5"
                || sign_key == "host" || sign_key[0] == 'x') {
                header_list.append(sign_key);
                http_request_info.append(sign_key).append("=")
                        .append(urlencode(iter->second.c_str()));
                header_list.append(";");
                http_request_info.append("&");
            }
        }
        if (!header_list.empty()) {
            header_list[header_list.size() - 1] = 0;
            http_request_info[http_request_info.size() - 1] = '\n';
        }
        //printf("%s\nEOF\n", http_request_info.c_str());
        char signed_time[SIGNLEN];
        int signed_time_len = snprintf(signed_time, SIGNLEN,
                                       "%lu;%lu", time(0) - 60, time(0) + expire);
        //snprintf(signed_time, SIGNLEN, "1510109254;1510109314");
        std::string signkey = hmac_sha1(secret_key.c_str(),
                                        signed_time, signed_time_len);
        str_to_sign.reserve(SIGNLEN);
        str_to_sign.append("sha1").append("\n")
                .append(signed_time).append("\n")
                .append(sha1(http_request_info.c_str(), http_request_info.size()))
                .append("\n");
        //printf("%s\nEOF\n", str_to_sign.c_str());
        char c_signature[SIGNLEN];
        snprintf(c_signature, SIGNLEN,
                 "q-sign-algorithm=sha1&q-ak=%s"
                 "&q-sign-time=%s&q-key-time=%s"
                 "&q-header-list=%s&q-url-param-list=%s&q-signature=%s",
                 secret_id.c_str(), signed_time, signed_time,
                 header_list.c_str(), uri_parm_list.c_str(),
                 hmac_sha1(signkey.c_str(), str_to_sign.c_str(),
                           str_to_sign.size()).c_str());
        return c_signature;
    }
}

// See https://developers.google.com/protocol-buffers/docs/encoding#varints
class SrsProtobufVarints
{
private:
    // See Go bits.Len64 of package math/bits.
    static int bits_len64(uint64_t x) {
        // See Go bits.len8tab of package math/bits.
        static uint8_t bits_len8tab[256] = {
                0x00, 0x01, 0x02, 0x02, 0x03, 0x03, 0x03, 0x03, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04,
                0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05,
                0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06,
                0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06,
                0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07,
                0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07,
                0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07,
                0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07,
                0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08,
                0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08,
                0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08,
                0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08,
                0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08,
                0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08,
                0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08,
                0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08,
        };

        int n = 0;
        if (x >= (uint64_t)1<<32) {
            x >>= 32;
            n = 32;
        }
        if (x >= (uint64_t)1<<16) {
            x >>= 16;
            n += 16;
        }
        if (x >= (uint64_t)1<<8) {
            x >>= 8;
            n += 8;
        }
        return n + int(bits_len8tab[x]);
    }
public:
    // See Go protowire.SizeVarint of package google.golang.org/protobuf/encoding/protowire
    static int sizeof_varint(uint64_t v) {
        int n = bits_len64(v);
        return int(9 * uint32_t(n) + 64) / 64;
    }
    // See Go protowire.AppendVarint of package google.golang.org/protobuf/encoding/protowire
    static srs_error_t encode(SrsBuffer* b, uint64_t v) {
        srs_error_t err = srs_success;

        if (!b->require(SrsProtobufVarints::sizeof_varint(v))) {
            return srs_error_new(ERROR_PB_NO_SPACE, "require %d only %d bytes", v, b->left());
        }

        if (v < (uint64_t)1<<7) {
            b->write_1bytes((uint8_t)v);
        } else if (v < (uint64_t)1<<14) {
            b->write_1bytes((uint8_t)(((v>>0)&0x7f)|0x80));
            b->write_1bytes((uint8_t)(v>>7));
        } else if (v < (uint64_t)1<<21) {
            b->write_1bytes((uint8_t)(((v>>0)&0x7f)|0x80));
            b->write_1bytes((uint8_t)(((v>>7)&0x7f)|0x80));
            b->write_1bytes((uint8_t)(((v>>14)&0x7f)|0x80));
            b->write_1bytes((uint8_t)(v>>21));
        } else if (v < (uint64_t)1<<35) {
            b->write_1bytes((uint8_t)(((v>>0)&0x7f)|0x80));
            b->write_1bytes((uint8_t)(((v>>7)&0x7f)|0x80));
            b->write_1bytes((uint8_t)(((v>>14)&0x7f)|0x80));
            b->write_1bytes((uint8_t)(((v>>21)&0x7f)|0x80));
            b->write_1bytes((uint8_t)(v>>28));
        } else if (v < (uint64_t)1<<42) {
            b->write_1bytes((uint8_t)(((v>>0)&0x7f)|0x80));
            b->write_1bytes((uint8_t)(((v>>7)&0x7f)|0x80));
            b->write_1bytes((uint8_t)(((v>>14)&0x7f)|0x80));
            b->write_1bytes((uint8_t)(((v>>21)&0x7f)|0x80));
            b->write_1bytes((uint8_t)(((v>>28)&0x7f)|0x80));
            b->write_1bytes((uint8_t)(v>>35));
        } else if (v < (uint64_t)1<<49) {
            b->write_1bytes((uint8_t)(((v>>0)&0x7f)|0x80));
            b->write_1bytes((uint8_t)(((v>>7)&0x7f)|0x80));
            b->write_1bytes((uint8_t)(((v>>14)&0x7f)|0x80));
            b->write_1bytes((uint8_t)(((v>>21)&0x7f)|0x80));
            b->write_1bytes((uint8_t)(((v>>28)&0x7f)|0x80));
            b->write_1bytes((uint8_t)(((v>>35)&0x7f)|0x80));
            b->write_1bytes((uint8_t)(v>>42));
        } else if(v < (uint64_t)1<<56) {
            b->write_1bytes((uint8_t)(((v>>0)&0x7f)|0x80));
            b->write_1bytes((uint8_t)(((v>>7)&0x7f)|0x80));
            b->write_1bytes((uint8_t)(((v>>14)&0x7f)|0x80));
            b->write_1bytes((uint8_t)(((v>>21)&0x7f)|0x80));
            b->write_1bytes((uint8_t)(((v>>28)&0x7f)|0x80));
            b->write_1bytes((uint8_t)(((v>>35)&0x7f)|0x80));
            b->write_1bytes((uint8_t)(((v>>42)&0x7f)|0x80));
            b->write_1bytes((uint8_t)(v>>49));
        } else if (v < (uint64_t)1<<63) {
            b->write_1bytes((uint8_t)(((v>>0)&0x7f)|0x80));
            b->write_1bytes((uint8_t)(((v>>7)&0x7f)|0x80));
            b->write_1bytes((uint8_t)(((v>>14)&0x7f)|0x80));
            b->write_1bytes((uint8_t)(((v>>21)&0x7f)|0x80));
            b->write_1bytes((uint8_t)(((v>>28)&0x7f)|0x80));
            b->write_1bytes((uint8_t)(((v>>35)&0x7f)|0x80));
            b->write_1bytes((uint8_t)(((v>>42)&0x7f)|0x80));
            b->write_1bytes((uint8_t)(((v>>49)&0x7f)|0x80));
            b->write_1bytes((uint8_t)(v>>56));
        } else {
            b->write_1bytes((uint8_t)(((v>>0)&0x7f)|0x80));
            b->write_1bytes((uint8_t)(((v>>7)&0x7f)|0x80));
            b->write_1bytes((uint8_t)(((v>>14)&0x7f)|0x80));
            b->write_1bytes((uint8_t)(((v>>21)&0x7f)|0x80));
            b->write_1bytes((uint8_t)(((v>>28)&0x7f)|0x80));
            b->write_1bytes((uint8_t)(((v>>35)&0x7f)|0x80));
            b->write_1bytes((uint8_t)(((v>>42)&0x7f)|0x80));
            b->write_1bytes((uint8_t)(((v>>49)&0x7f)|0x80));
            b->write_1bytes((uint8_t)(((v>>56)&0x7f)|0x80));
            b->write_1bytes((uint8_t)1);
        }

        return err;
    }
};

class SrsProtobufString
{
public:
    // See Go protowire.SizeBytes of package google.golang.org/protobuf/encoding/protowire
    static int sizeof_string(const std::string& v) {
        uint64_t n = v.length();
        return SrsProtobufVarints::sizeof_varint(uint64_t(n)) + n;
    }
    // See Go protowire.AppendString of package google.golang.org/protobuf/encoding/protowire
    static srs_error_t encode(SrsBuffer* b, const std::string& v) {
        srs_error_t  err = srs_success;

        uint64_t n = v.length();
        if ((err = SrsProtobufVarints::encode(b, n)) != srs_success) {
            return srs_error_wrap(err, "string size %d", n);
        }

        if (!b->require(n)) {
            return srs_error_new(ERROR_PB_NO_SPACE, "require %d only %d byte", n, b->left());
        }
        b->write_string(v);

        return err;
    }
};

// See https://cloud.tencent.com/document/api/614/16873
class SrsClsLogContent
{
private:
    // required string key = 1;
    std::string key_;
    // required string value = 2;
    std::string value_;
public:
    SrsClsLogContent();
    virtual ~SrsClsLogContent();
public:
    SrsClsLogContent* set_key(std::string v);
    SrsClsLogContent* set_value(std::string v);
public:
    virtual uint64_t nb_bytes();
    srs_error_t encode(SrsBuffer* b);
};

// See https://cloud.tencent.com/document/api/614/16873
class SrsClsLog
{
private:
    // required int64 time = 1;
    int64_t time_;
    // repeated Content contents= 2;
    std::vector<SrsClsLogContent*> contents_;
public:
    SrsClsLog();
    virtual ~SrsClsLog();
public:
    SrsClsLogContent* add_content();
    SrsClsLog* set_time(int64_t v);
public:
    virtual uint64_t nb_bytes();
    srs_error_t encode(SrsBuffer* b);
};

// See https://cloud.tencent.com/document/api/614/16873
class SrsClsLogGroup
{
private:
    // repeated Log logs= 1;
    std::vector<SrsClsLog*> logs_;
    // optional string source = 4;
    std::string source_;
public:
    SrsClsLogGroup();
    virtual ~SrsClsLogGroup();
public:
    SrsClsLogGroup* set_source(std::string v);
    SrsClsLog* add_log();
public:
    virtual uint64_t nb_bytes();
    srs_error_t encode(SrsBuffer* b);
};

// See https://cloud.tencent.com/document/api/614/16873
class SrsClsLogGroupList
{
private:
    // repeated LogGroup logGroupList = 1;
    std::vector<SrsClsLogGroup*> groups_;
public:
    SrsClsLogGroupList();
    virtual ~SrsClsLogGroupList();
public:
    SrsClsLogGroup* add_log_group();
public:
    virtual uint64_t nb_bytes();
    srs_error_t encode(SrsBuffer* b);
};

SrsClsLogContent::SrsClsLogContent()
{
}

SrsClsLogContent::~SrsClsLogContent()
{
}

SrsClsLogContent* SrsClsLogContent::set_key(std::string v)
{
    key_ = v;
    return this;
}

SrsClsLogContent* SrsClsLogContent::set_value(std::string v)
{
    value_ = v;
    return this;
}

uint64_t SrsClsLogContent::nb_bytes()
{
    uint64_t  nn = 1 + SrsProtobufString::sizeof_string(key_);
    nn += 1 + SrsProtobufString::sizeof_string(value_);
    return nn;
}

srs_error_t SrsClsLogContent::encode(SrsBuffer* b)
{
    srs_error_t err = srs_success;

    // Encode the field key as [ID=1, TYPE=2(Length delimited)]
    if (!b->require(1)) {
        return srs_error_new(ERROR_PB_NO_SPACE, "require 1 byte");
    }
    b->write_1bytes(0x0a);

    if ((err = SrsProtobufString::encode(b, key_)) != srs_success) {
        return srs_error_wrap(err, "encode key=%s", key_.c_str());
    }

    // Encode the field value as [ID=2, TYPE=2(Length delimited)]
    if (!b->require(1)) {
        return srs_error_new(ERROR_PB_NO_SPACE, "require 1 byte");
    }
    b->write_1bytes(0x12);

    if ((err = SrsProtobufString::encode(b, value_)) != srs_success) {
        return srs_error_wrap(err, "encode value=%s", value_.c_str());
    }

    return err;
}

SrsClsLog::SrsClsLog()
{
}

SrsClsLog::~SrsClsLog()
{
    for (std::vector<SrsClsLogContent*>::iterator it = contents_.begin(); it != contents_.end(); ++it) {
        SrsClsLogContent* content = *it;
        srs_freep(content);
    }
}

SrsClsLogContent* SrsClsLog::add_content()
{
    SrsClsLogContent* content = new SrsClsLogContent();
    contents_.push_back(content);
    return content;
}

SrsClsLog* SrsClsLog::set_time(int64_t v)
{
    time_ = v;
    return this;
}

uint64_t SrsClsLog::nb_bytes()
{
    uint64_t nn = 1 + SrsProtobufVarints::sizeof_varint(time_);

    for (std::vector<SrsClsLogContent*>::iterator it = contents_.begin(); it != contents_.end(); ++it) {
        SrsClsLogContent* content = *it;
        uint64_t size = content->nb_bytes();
        nn += 1 + SrsProtobufVarints::sizeof_varint(size) + size;
    }

    return nn;
}

srs_error_t SrsClsLog::encode(SrsBuffer* b)
{
    srs_error_t  err = srs_success;

    // Encode the field time as [ID=1, TYPE=0(Varint)]
    if (!b->require(1)) {
        return srs_error_new(ERROR_PB_NO_SPACE, "require 1 byte");
    }
    b->write_1bytes(0x08);

    if ((err = SrsProtobufVarints::encode(b, time_)) != srs_success) {
        return srs_error_wrap(err, "encode time");
    }

    // Encode each content.
    for (std::vector<SrsClsLogContent*>::iterator it = contents_.begin(); it != contents_.end(); ++it) {
        SrsClsLogContent* content = *it;

        // Encode the field contents as [ID=2, TYPE=2(Length delimited)]
        if (!b->require(1)) {
            return srs_error_new(ERROR_PB_NO_SPACE, "require 1 byte");
        }
        b->write_1bytes(0x12);

        // Encode the varint size of children.
        uint64_t size = content->nb_bytes();
        if ((err = SrsProtobufVarints::encode(b, size)) != srs_success) {
            return srs_error_wrap(err, "encode size=%d", (int)size);
        }

        // Encode the content itself.
        if ((err = content->encode(b)) != srs_success) {
            return srs_error_wrap(err, "encode content");
        }
    }

    return err;
}

SrsClsLogGroup::SrsClsLogGroup()
{
}

SrsClsLogGroup::~SrsClsLogGroup()
{
    for (std::vector<SrsClsLog*>::iterator it = logs_.begin(); it != logs_.end(); ++it) {
        SrsClsLog* log = *it;
        srs_freep(log);
    }
}

SrsClsLogGroup* SrsClsLogGroup::set_source(std::string v)
{
    source_ = v;
    return this;
}

SrsClsLog* SrsClsLogGroup::add_log()
{
    SrsClsLog* log = new SrsClsLog();
    logs_.push_back(log);
    return log;
}

uint64_t SrsClsLogGroup::nb_bytes()
{
    uint64_t nn = 0;
    for (std::vector<SrsClsLog*>::iterator it = logs_.begin(); it != logs_.end(); ++it) {
        SrsClsLog* log = *it;
        uint64_t size = log->nb_bytes();
        nn += 1 + SrsProtobufVarints::sizeof_varint(size) + size;
    }

    nn += 1 + SrsProtobufString::sizeof_string(source_);
    return nn;
}

srs_error_t SrsClsLogGroup::encode(SrsBuffer* b)
{
    srs_error_t err = srs_success;

    // Encode each log.
    for (std::vector<SrsClsLog*>::iterator it = logs_.begin(); it != logs_.end(); ++it) {
        SrsClsLog* log = *it;

        // Encode the field logs as [ID=1, TYPE=2(Length delimited)]
        if (!b->require(1)) {
            return srs_error_new(ERROR_PB_NO_SPACE, "require 1 byte");
        }
        b->write_1bytes(0x0a);

        // Encode the varint size of children.
        uint64_t size = log->nb_bytes();
        if ((err = SrsProtobufVarints::encode(b, size)) != srs_success) {
            return srs_error_wrap(err, "encode size=%d", (int)size);
        }

        // Encode the log itself.
        if ((err = log->encode(b)) != srs_success) {
            return srs_error_wrap(err, "encode log");
        }
    }

    // Encode the field source as [ID=4, TYPE=2(Length delimited)]
    if (!b->require(1)) {
        return srs_error_new(ERROR_PB_NO_SPACE, "require 1 byte");
    }
    b->write_1bytes(0x22);

    if ((err = SrsProtobufString::encode(b, source_)) != srs_success) {
        return srs_error_wrap(err, "encode source=%s", source_.c_str());
    }

    return err;
}

SrsClsLogGroupList::SrsClsLogGroupList()
{
}

SrsClsLogGroupList::~SrsClsLogGroupList()
{
    for (std::vector<SrsClsLogGroup*>::iterator it = groups_.begin(); it != groups_.end(); ++it) {
        SrsClsLogGroup* group = *it;
        srs_freep(group);
    }
}

SrsClsLogGroup* SrsClsLogGroupList::add_log_group()
{
    SrsClsLogGroup* group = new SrsClsLogGroup();
    groups_.push_back(group);
    return group;
}

uint64_t SrsClsLogGroupList::nb_bytes()
{
    uint64_t nn = 0;
    for (std::vector<SrsClsLogGroup*>::iterator it = groups_.begin(); it != groups_.end(); ++it) {
        SrsClsLogGroup* group = *it;
        uint64_t size = group->nb_bytes();
        nn += 1 + SrsProtobufVarints::sizeof_varint(size) + size;
    }
    return nn;
}

srs_error_t SrsClsLogGroupList::encode(SrsBuffer* b)
{
    srs_error_t err = srs_success;

    // Encode each group.
    for (std::vector<SrsClsLogGroup*>::iterator it = groups_.begin(); it != groups_.end(); ++it) {
        SrsClsLogGroup* group = *it;

        // Encode the field groups as [ID=1, TYPE=2(Length delimited)]
        if (!b->require(1)) {
            return srs_error_new(ERROR_PB_NO_SPACE, "require 1 byte");
        }
        b->write_1bytes(0x0a);

        // Encode the varint size of children.
        uint64_t size = group->nb_bytes();
        if ((err = SrsProtobufVarints::encode(b, size)) != srs_success) {
            return srs_error_wrap(err, "encode size=%d", (int)size);
        }

        // Encode the log group itself.
        if ((err = group->encode(b)) != srs_success) {
            return srs_error_wrap(err, "encode group");
        }
    }

    return err;
}

SrsClsSugar::SrsClsSugar()
{
    log_groups_ = new SrsClsLogGroupList();
    log_group_ = log_groups_->add_log_group();
    log_ = log_group_->add_log();

    log_group_->set_source(srs_get_public_internet_address(true));
    log_->set_time(srs_get_system_time() / SRS_UTIME_SECONDS);
    kv("agent", RTMP_SIG_SRS_SERVER);

    string label = _srs_cls->label();
    if (!label.empty()) {
        kv("label", label);
    }

    string tag = _srs_cls->tag();
    if (!tag.empty()) {
        kv("tag", tag);
    }

    string server_id = SrsStatistic::instance()->server_id();
    if (!server_id.empty()) {
        kv("id", server_id);
    }
}

SrsClsSugar::~SrsClsSugar()
{
    srs_freep(log_groups_);
}

SrsClsSugar* SrsClsSugar::kv(std::string k, std::string v)
{
    log_->add_content()->set_key(k)->set_value(v);
    return this;
}

SrsClsSugar* SrsClsSugar::kvf(std::string k, const char* fmt, ...)
{
    static int LOG_MAX_SIZE = 4096;
    static char* buf = new char[LOG_MAX_SIZE];

    va_list ap;
    va_start(ap, fmt);
    int r0 = vsnprintf(buf, LOG_MAX_SIZE, fmt, ap);
    va_end(ap);

    // Something not expected, drop the log. If error, it might be 0 or negative value. If greater or equals to the
    // LOG_MAX_SIZE, means need more buffers to write the data and the last byte might be 0. If success, return the
    // number of characters printed, not including the trailing 0.
    if (r0 <= 0 || r0 >= LOG_MAX_SIZE) {
        return this;
    }

    string v = string(buf, r0);
    return kv(k, v);
}

uint64_t SrsClsSugar::nb_bytes()
{
    return log_groups_->nb_bytes();
}

srs_error_t SrsClsSugar::encode(SrsBuffer* b)
{
    return log_groups_->encode(b);
}

SrsClsSugars::SrsClsSugars()
{
}

SrsClsSugars::~SrsClsSugars()
{
    for (vector<SrsClsSugar*>::iterator it = sugars.begin(); it != sugars.end(); ++it) {
        SrsClsSugar* sugar = *it;
        srs_freep(sugar);
    }
}

uint64_t SrsClsSugars::nb_bytes()
{
    uint64_t size = 0;
    for (vector<SrsClsSugar*>::iterator it = sugars.begin(); it != sugars.end(); ++it) {
        SrsClsSugar* sugar = *it;
        size += sugar->nb_bytes();
    }
    return size;
}

srs_error_t SrsClsSugars::encode(SrsBuffer* b)
{
    srs_error_t err = srs_success;

    for (vector<SrsClsSugar*>::iterator it = sugars.begin(); it != sugars.end(); ++it) {
        SrsClsSugar* sugar = *it;
        if ((err = sugar->encode(b)) != srs_success) {
            return srs_error_wrap(err, "encode %d sugars", (int)sugars.size());
        }
    }

    return err;
}

SrsClsSugar* SrsClsSugars::create()
{
    SrsClsSugar* sugar = new SrsClsSugar();
    sugars.push_back(sugar);
    return sugar;
}

SrsClsSugars* SrsClsSugars::slice(int max_size)
{
    SrsClsSugars* v = new SrsClsSugars();

    uint64_t v_size = 0;
    for (vector<SrsClsSugar*>::iterator it = sugars.begin(); it != sugars.end();) {
        SrsClsSugar* sugar = *it;

        // Always push at least one elem.
        it = sugars.erase(it);
        v->sugars.push_back(sugar);

        // Util exceed the max size.
        v_size += sugar->nb_bytes();
        if (v_size > max_size) {
            break;
        }
    }

    return v;
}

bool SrsClsSugars::empty()
{
    return sugars.empty();
}

int SrsClsSugars::size()
{
    return (int)sugars.size();
}

SrsClsClient* _srs_cls = NULL;

SrsClsClient::SrsClsClient()
{
    enabled_ = false;
    stat_heartbeat_ = false;
    stat_streams_ = false;
    debug_logging_ = false;
    heartbeat_ratio_ = 0;
    streams_ratio_ = 0;
}

SrsClsClient::~SrsClsClient()
{
}

bool SrsClsClient::enabled()
{
    return enabled_;
}

bool SrsClsClient::stat_heartbeat()
{
    return stat_heartbeat_;
}

bool SrsClsClient::stat_streams()
{
    return stat_streams_;
}

int SrsClsClient::heartbeat_ratio()
{
    return heartbeat_ratio_;
}

int SrsClsClient::streams_ratio()
{
    return streams_ratio_;
}

string SrsClsClient::label()
{
    return label_;
}

string SrsClsClient::tag()
{
    return tag_;
}

srs_error_t SrsClsClient::initialize()
{
    srs_error_t err = srs_success;

    enabled_ = _srs_config->get_tencentcloud_cls_enabled();
    if (!enabled_) {
        srs_trace("TencentCloud CLS is disabled");
        return err;
    }

    label_ = _srs_config->get_tencentcloud_cls_label();
    tag_ = _srs_config->get_tencentcloud_cls_tag();
    stat_heartbeat_ = _srs_config->get_tencentcloud_cls_stat_heartbeat();
    stat_streams_ = _srs_config->get_tencentcloud_cls_stat_streams();
    debug_logging_ = _srs_config->get_tencentcloud_cls_debug_logging();
    heartbeat_ratio_ = srs_max(1, _srs_config->get_tencentcloud_cls_heartbeat_ratio());
    streams_ratio_ = srs_max(1, _srs_config->get_tencentcloud_cls_streams_ratio());

    secret_id_ = _srs_config->get_tencentcloud_cls_secret_id();
    if (secret_id_.empty()) {
        return srs_error_new(ERROR_CLS_INVALID_CONFIG, "CLS no config for secret_id");
    }

    string secret_key = _srs_config->get_tencentcloud_cls_secret_key();
    if (secret_key.empty()) {
        return srs_error_new(ERROR_CLS_INVALID_CONFIG, "CLS no config for secret_key");
    }

    endpoint_ = _srs_config->get_tencentcloud_cls_endpoint();
    if (endpoint_.empty()) {
        return srs_error_new(ERROR_CLS_INVALID_CONFIG, "CLS no config for endpoint");
    }

    topic_ = _srs_config->get_tencentcloud_cls_topic_id();
    if (topic_.empty()) {
        return srs_error_new(ERROR_CLS_INVALID_CONFIG, "CLS no config for topic_id");
    }

    srs_trace("Initialize TencentCloud CLS label=%s, tag=%s, secret_id=%dB, secret_key=%dB, endpoint=%s, topic=%s, heartbeat=%d/%d, streams=%d/%d debug_logging=%d",
        label_.c_str(), tag_.c_str(), secret_id_.length(), secret_key.length(), endpoint_.c_str(), topic_.c_str(), stat_heartbeat_, heartbeat_ratio_, stat_streams_, streams_ratio_, debug_logging_);

    return err;
}

srs_error_t SrsClsClient::send_log(ISrsEncoder* sugar, int count, int total)
{
    srs_error_t err = srs_success;

    uint64_t size = sugar->nb_bytes();
    // Max size is 5MB, error is 403:LogSizeExceed, see https://cloud.tencent.com/document/api/614/12402
    if (size >= 5 * 1024 * 1024) {
        return srs_error_new(ERROR_CLS_EXCEED_SIZE, "exceed 5MB actual %d", size);
    }

    char* buf = new char[size];
    SrsAutoFreeA(char, buf);

    memset(buf, 0, size);
    SrsBuffer b(buf, size);
    if ((err = sugar->encode(&b)) != srs_success) {
        return srs_error_wrap(err, "encode log");
    }

    string body(buf, size);

    // Write a CLS log to service specified by url.
    string url = "http://" + endpoint_ + ":80/structuredlog?topic_id=" + topic_;

    SrsHttpUri uri;
    if ((err = uri.initialize(url)) != srs_success) {
        return srs_error_wrap(err, "http: post failed. url=%s", url.c_str());
    }

    SrsHttpClient http;
    if ((err = http.initialize(uri.get_schema(), uri.get_host(), uri.get_port())) != srs_success) {
        return srs_error_wrap(err, "http: init client");
    }

    // Sign the request, see https://cloud.tencent.com/document/product/614/56475
    if (true) {
        map<string, string> params;
        params["topic_id"] = topic_;

        map<string, string> headers;
        headers["Host"] = uri.get_host();
        headers["Content-Type"] = "application/x-protobuf";
        http.set_header("Content-Type", "application/x-protobuf");

        string method = "POST";
        string secret_key = _srs_config->get_tencentcloud_cls_secret_key();
        std::string signature = tencentcloud_api_sign::signature(
            secret_id_, secret_key, method, uri.get_path(), params, headers, 300 // Expire in seconds
        );
        headers["Authorization"] = signature;
        http.set_header("Authorization", signature);
    }

    string path = uri.get_path();
    if (!uri.get_query().empty()) {
        path += "?" + uri.get_query();
    }

    // Start request and parse response.
    ISrsHttpMessage* msg = NULL;
    if ((err = http.post(path, body, &msg)) != srs_success) {
        return srs_error_wrap(err, "http: client post");
    }
    SrsAutoFree(ISrsHttpMessage, msg);

    string res;
    uint16_t code = msg->status_code();
    if ((err = msg->body_read_all(res)) != srs_success) {
        return srs_error_wrap(err, "http: body read");
    }

    // ensure the http status is ok.
    if (code != SRS_CONSTS_HTTP_OK && code != SRS_CONSTS_HTTP_Created) {
        return srs_error_new(ERROR_HTTP_STATUS_INVALID, "http: status %d, body is %s", code, res.c_str());
    }

    string request_id = msg->header()->get("X-Cls-Requestid");
    if (request_id.empty() && !debug_logging_) {
        srs_warn("no CLS requestId for log %dB", body.length());
    }

    if (debug_logging_) {
        srs_trace("CLS write logs=%d/%d, size=%dB, request_id=%s", count, total, body.length(), request_id.c_str());
    }

    return err;
}

// For each upload, never exceed 2MB, to avoid burst of CPU or network usage.
#define SRS_CLS_BATCH_MAX_LOG_SIZE 2 * 1024 * 1024

srs_error_t SrsClsClient::send_logs(SrsClsSugars* sugars)
{
    srs_error_t err = srs_success;

    // Never do infinite loop, limit to a max loop and drop logs if exceed.
    int total = sugars->size();
    for (int i = 0; i < 128 && !sugars->empty(); ++i) {
        SrsClsSugars* v = sugars->slice(SRS_CLS_BATCH_MAX_LOG_SIZE);
        SrsAutoFree(SrsClsSugars, v);

        if ((err = send_log((ISrsEncoder*)v, v->size(), total)) != srs_success) {
            return srs_error_wrap(err, "send %d/%d/%d logs", v->size(), i, total);
        }
    }

    return err;
}

srs_error_t srs_cls_dump_summaries(SrsClsSugars* sugars)
{
    srs_error_t err = srs_success;

    // Ignore if disabled.
    if (!_srs_cls->enabled() || !_srs_cls->stat_heartbeat()) {
        return err;
    }

    // Whether it's time to report heartbeat.
    static int nn_heartbeat = -1;
    bool interval_ok = nn_heartbeat == -1 || ++nn_heartbeat >= _srs_cls->heartbeat_ratio();
    if (interval_ok) {
        nn_heartbeat = 0;
    }
    if (!interval_ok) {
        return err;
    }

    SrsClsSugar* sugar = sugars->create();
    sugar->kv("hint", "summary");
    sugar->kv("version", RTMP_SIG_SRS_VERSION);
    sugar->kvf("pid", "%d", getpid());

    // Server ID to identify logs from a set of servers' logs.
    SrsStatistic::instance()->dumps_cls_summaries(sugar);

    SrsProcSelfStat* u = srs_get_self_proc_stat();
    if (u->ok) {
        // The cpu usage of SRS, 1 means 1/1000
        if (u->percent > 0) {
            sugar->kvf("cpu", "%d", (int) (u->percent * 1000));
        }
    }

    SrsPlatformInfo* p = srs_get_platform_info();
    if (p->ok) {
        // The uptime of SRS, in seconds.
        if (p->srs_startup_time > 0) {
            sugar->kvf("uptime", "%d", (int) ((srs_get_system_time() - p->srs_startup_time) / SRS_UTIME_SECONDS));
        }
        // The load of system, load every 1 minute, 1 means 1/1000.
        if (p->load_one_minutes > 0) {
            sugar->kvf("load", "%d", (int) (p->load_one_minutes * 1000));
        }
    }

    SrsRusage* r = srs_get_system_rusage();
    SrsMemInfo* m = srs_get_meminfo();
    if (r->ok && m->ok) {
        float self_mem_percent = 0;
        if (m->MemTotal > 0) {
            self_mem_percent = (float)(r->r.ru_maxrss / (double)m->MemTotal);
        }

        // The memory of SRS, 1 means 1/1000
        if (self_mem_percent > 0) {
            sugar->kvf("mem", "%d", (int) (self_mem_percent * 1000));
        }
    }

    SrsProcSystemStat* s = srs_get_system_proc_stat();
    if (s->ok) {
        // The cpu usage of system, 1 means 1/1000
        if (s->percent > 0) {
            sugar->kvf("cpu2", "%d", (int) (s->percent * 1000));
        }
    }

    SrsNetworkRtmpServer* nrs = srs_get_network_rtmp_server();
    if (nrs->ok) {
        // The number of connections of SRS.
        if (nrs->nb_conn_srs > 0) {
            sugar->kvf("conn", "%d", nrs->nb_conn_srs);
        }
        // The number of connections of system.
        if (nrs->nb_conn_sys > 0) {
            sugar->kvf("conn2", "%d", nrs->nb_conn_sys);
        }
        // The received kbps in 30s of SRS.
        if (nrs->rkbps_30s > 0) {
            sugar->kvf("recv", "%d", nrs->rkbps_30s);
        }
        // The sending out kbps in 30s of SRS.
        if (nrs->skbps_30s > 0) {
            sugar->kvf("send", "%d", nrs->skbps_30s);
        }
    }

    return err;
}

srs_error_t srs_cls_dump_streams(SrsClsSugars* sugars)
{
    srs_error_t err = srs_success;

    // Ignore if disabled.
    if (!_srs_cls->enabled() || !_srs_cls->stat_streams()) {
        return err;
    }

    // Whether it's time to report streams.
    static int nn_streams = -1;
    bool interval_ok = nn_streams == -1 || ++nn_streams >= _srs_cls->streams_ratio();
    if (interval_ok) {
        nn_streams = 0;
    }
    if (!interval_ok) {
        return err;
    }

    // Dumps all streams as sugars.
    SrsStatistic::instance()->dumps_cls_streams(sugars);

    return err;
}

srs_error_t srs_cls_report()
{
    srs_error_t err = srs_success;

    SrsClsSugars sugars;

    if ((err = srs_cls_dump_summaries(&sugars)) != srs_success) {
        return srs_error_wrap(err, "dump summary");
    }

    if ((err = srs_cls_dump_streams(&sugars)) != srs_success) {
        return srs_error_wrap(err, "dump streams");
    }

    if ((err = _srs_cls->send_logs(&sugars)) != srs_success) {
        return srs_error_wrap(err, "cls");
    }

    return err;
}


