#ifndef CERTS_H
#define CERTS_H

#include <stdint.h>
#include <stddef.h>

extern const uint8_t cacert_pem[];
extern const size_t cacert_pem_len;

extern const uint8_t client_pem[];
extern const size_t client_pem_len;

extern const uint8_t client_key_pem[];
extern const size_t client_key_pem_len;

#endif /* CERTS_H */
