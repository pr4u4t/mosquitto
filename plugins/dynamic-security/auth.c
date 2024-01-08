/*
Copyright (c) 2020 Roger Light <roger@atchoo.org>

All rights reserved. This program and the accompanying materials
are made available under the terms of the Eclipse Public License 2.0
and Eclipse Distribution License v1.0 which accompany this distribution.

The Eclipse Public License is available at
   https://www.eclipse.org/legal/epl-2.0/
and the Eclipse Distribution License is available at
  http://www.eclipse.org/org/documents/edl-v10.php.

SPDX-License-Identifier: EPL-2.0 OR BSD-3-Clause

Contributors:
   Roger Light - initial implementation and documentation.
*/

#include "config.h"

#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include "dynamic_security.h"
#include "mosquitto.h"
#include "mosquitto_broker.h"


/* ################################################################
 * #
 * # Base64 encoding/decoding
 * #
 * ################################################################ */

int dynsec_auth__base64_encode(unsigned char *in, int in_len, char **encoded)
{
	BIO *bmem, *b64;
	BUF_MEM *bptr = NULL;

	if(in_len < 0) return 1;

	b64 = BIO_new(BIO_f_base64());
	if(b64 == NULL) return 1;

	BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
	bmem = BIO_new(BIO_s_mem());
	if(bmem == NULL){
		BIO_free_all(b64);
		return 1;
	}
	b64 = BIO_push(b64, bmem);
	BIO_write(b64, in, in_len);
	if(BIO_flush(b64) != 1){
		BIO_free_all(b64);
		return 1;
	}
	BIO_get_mem_ptr(b64, &bptr);
	*encoded = mosquitto_malloc(bptr->length+1);
	if(!(*encoded)){
		BIO_free_all(b64);
		return 1;
	}
	memcpy(*encoded, bptr->data, bptr->length);
	(*encoded)[bptr->length] = '\0';
	BIO_free_all(b64);

	return 0;
}


int dynsec_auth__base64_decode(char *in, unsigned char **decoded, int *decoded_len)
{
	BIO *bmem, *b64;
	size_t slen;

	slen = strlen(in);

	b64 = BIO_new(BIO_f_base64());
	if(!b64){
		return 1;
	}
	BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);

	bmem = BIO_new(BIO_s_mem());
	if(!bmem){
		BIO_free_all(b64);
		return 1;
	}
	b64 = BIO_push(b64, bmem);
	BIO_write(bmem, in, (int)slen);

	if(BIO_flush(bmem) != 1){
		BIO_free_all(b64);
		return 1;
	}
	*decoded = mosquitto_calloc(slen, 1);
	if(!(*decoded)){
		BIO_free_all(b64);
		return 1;
	}
	*decoded_len =  BIO_read(b64, *decoded, (int)slen);
	BIO_free_all(b64);

	if(*decoded_len <= 0){
		mosquitto_free(*decoded);
		*decoded = NULL;
		*decoded_len = 0;
		return 1;
	}

	return 0;
}


/* ################################################################
 * #
 * # Password functions
 * #
 * ################################################################ */

int dynsec_auth__pw_hash(struct dynsec__client *client, const char *password, unsigned char *password_hash, int password_hash_len, bool new_password)
{
	const EVP_MD *digest;
	int iterations;

	if(new_password){
		if(RAND_bytes(client->pw.salt, sizeof(client->pw.salt)) != 1){
			return MOSQ_ERR_UNKNOWN;
		}
		iterations = PW_DEFAULT_ITERATIONS;
	}else{
		iterations = client->pw.iterations;
	}
	if(iterations < 1){
		return MOSQ_ERR_INVAL;
	}
	client->pw.iterations = iterations;

	digest = EVP_get_digestbyname("sha512");
	if(!digest){
		return MOSQ_ERR_UNKNOWN;
	}

	return !PKCS5_PBKDF2_HMAC(password, (int)strlen(password),
			client->pw.salt, sizeof(client->pw.salt), iterations,
			digest, password_hash_len, password_hash);
}


/* ################################################################
 * #
 * # Username/password check
 * #
 * ################################################################ */

static int memcmp_const(const void *a, const void *b, size_t len)
{
	size_t i;
	int rc = 0;

	if(!a || !b) return 1;

	for(i=0; i<len; i++){
		rc |= ((char *)a)[i] ^ ((char *)b)[i];
	}
	return rc;
}


int dynsec_auth__basic_auth_callback(int event, void *event_data, void *userdata)
{
	struct mosquitto_evt_basic_auth *ed = event_data;
	struct dynsec__client *client;
	unsigned char password_hash[64]; /* For SHA512 */
	const char *clientid;

	UNUSED(event);
	UNUSED(userdata);

	if(ed->username == NULL || ed->password == NULL) return MOSQ_ERR_PLUGIN_DEFER;

	client = dynsec_clients__find(ed->username);
	if(client){
		if(client->disabled){
			return MOSQ_ERR_AUTH;
		}
		if(client->clientid){
			clientid = mosquitto_client_id(ed->client);
			if(clientid == NULL || strcmp(client->clientid, clientid)){
				return MOSQ_ERR_AUTH;
			}
		}
		if(client->pw.valid && dynsec_auth__pw_hash(client, ed->password, password_hash, sizeof(password_hash), false) == MOSQ_ERR_SUCCESS){
			if(memcmp_const(client->pw.password_hash, password_hash, sizeof(password_hash)) == 0){
#ifdef WITH_LUA
				if(userdata != NULL){
					lua_settop((lua_State*) userdata,1);
					lua_pushvalue((lua_State*) userdata, -1);

					const char *cid = mosquitto_client_id(ed->client);
					const char *addr = mosquitto_client_address(ed->client);
					mosquitto_log_printf(MOSQ_LOG_INFO, "Info: client: %s %s connected", cid, addr);
					lua_pushvalue((lua_State*) userdata,-1);

					lua_newtable((lua_State*) userdata);

					lua_pushnumber((lua_State*) userdata, 0);
					lua_pushstring((lua_State*) userdata, "mosquitto");
					lua_settable((lua_State*) userdata, -3);

					lua_pushnumber((lua_State*) userdata, 1);
					lua_pushstring((lua_State*) userdata, "CONNECTED");
					lua_settable((lua_State*) userdata, -3);

					lua_pushnumber((lua_State*) userdata, 2);
					lua_pushstring((lua_State*) userdata, cid);
					lua_settable((lua_State*) userdata, -3);

					lua_pushnumber((lua_State*) userdata, 3);
					lua_pushstring((lua_State*) userdata, addr);
					lua_settable((lua_State*) userdata, -3);

					lua_setglobal((lua_State*) userdata, "arg");

					if(lua_pcall((lua_State*) userdata, 0, 1, 0) != 0){
						mosquitto_log_printf(MOSQ_LOG_ERR, "Error: auth lua pcall failed: %s.", lua_tostring((lua_State*) userdata,-1));
						return MOSQ_ERR_UNKNOWN;
					} else {
						if(lua_isboolean((lua_State*) userdata, -1) == 1){
							lua_Integer ret_val = luaL_checkinteger((lua_State*) userdata, -1);
							mosquitto_log_printf(MOSQ_LOG_DEBUG, "Debug: auth Lua handler returned: %d.", ret_val);
						}else{
							mosquitto_log_printf(MOSQ_LOG_DEBUG, "Debug: auth lua_pcall invalid return type");
						}
					}
				}
#endif

				return MOSQ_ERR_SUCCESS;
			}else{
				return MOSQ_ERR_AUTH;
			}
		}else{
			return MOSQ_ERR_PLUGIN_DEFER;
		}
	}else{
		return MOSQ_ERR_PLUGIN_DEFER;
	}
}
