/*
 * Copyright (C) 2010 Martin Willi
 * Copyright (C) 2010 revosec AG
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <http://www.fsf.org/copyleft/gpl.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#include "tls_crypto.h"

#include <daemon.h>

typedef struct private_tls_crypto_t private_tls_crypto_t;

/**
 * Private data of an tls_crypto_t object.
 */
struct private_tls_crypto_t {

	/**
	 * Public tls_crypto_t interface.
	 */
	tls_crypto_t public;

	/**
	 * List of supported/acceptable cipher suites
	 */
	tls_cipher_suite_t *suites;

	/**
	 * Number of supported suites
	 */
	int suite_count;

	/**
	 * Selected cipher suite
	 */
	tls_cipher_suite_t suite;

	/**
	 * TLS context
	 */
	tls_t *tls;

	/**
	 * Connection state TLS PRF
	 */
	tls_prf_t *prf;

	/**
	 * Signer instance for inbound traffic
	 */
	signer_t *signer_in;

	/**
	 * Signer instance for outbound traffic
	 */
	signer_t *signer_out;

	/**
	 * Crypter instance for inbound traffic
	 */
	crypter_t *crypter_in;

	/**
	 * Crypter instance for outbound traffic
	 */
	crypter_t *crypter_out;

	/**
	 * IV for input decryption, if < TLSv1.2
	 */
	chunk_t iv_in;

	/**
	 * IV for output decryption, if < TLSv1.2
	 */
	chunk_t iv_out;
};

typedef struct {
	tls_cipher_suite_t suite;
	hash_algorithm_t hash;
	pseudo_random_function_t prf;
	integrity_algorithm_t mac;
	encryption_algorithm_t encr;
	size_t encr_size;
} suite_algs_t;

/**
 * Mapping suites to a set of algorithms
 */
static suite_algs_t suite_algs[] = {
	{ TLS_RSA_WITH_NULL_MD5,
		HASH_MD5,
		PRF_HMAC_MD5,
		AUTH_HMAC_MD5_128,
		ENCR_NULL, 0
	},
	{ TLS_RSA_WITH_NULL_SHA,
		HASH_SHA1,
		PRF_HMAC_SHA1,
		AUTH_HMAC_SHA1_160,
		ENCR_NULL, 0
	},
	{ TLS_RSA_WITH_NULL_SHA256,
		HASH_SHA256,
		PRF_HMAC_SHA2_256,
		AUTH_HMAC_SHA2_256_256,
		ENCR_NULL, 0
	},
	{ TLS_RSA_WITH_AES_128_CBC_SHA,
		HASH_SHA1,
		PRF_HMAC_SHA1,
		AUTH_HMAC_SHA1_160,
		ENCR_AES_CBC, 16
	},
	{ TLS_RSA_WITH_AES_256_CBC_SHA,
		HASH_SHA1,
		PRF_HMAC_SHA1,
		AUTH_HMAC_SHA1_160,
		ENCR_AES_CBC, 32
	},
	{ TLS_RSA_WITH_3DES_EDE_CBC_SHA,
		HASH_SHA1,
		PRF_HMAC_SHA1,
		AUTH_HMAC_SHA1_160,
		ENCR_3DES, 0
	},
	{ TLS_RSA_WITH_AES_128_CBC_SHA256,
		HASH_SHA256,
		PRF_HMAC_SHA2_256,
		AUTH_HMAC_SHA2_256_256,
		ENCR_AES_CBC, 16
	},
};

/**
 * Look up algoritms by a suite
 */
static suite_algs_t *find_suite(tls_cipher_suite_t suite)
{
	int i;

	for (i = 0; i < countof(suite_algs); i++)
	{
		if (suite_algs[i].suite == suite)
		{
			return &suite_algs[i];
		}
	}
	return NULL;
}

/**
 * Initialize the cipher suite list
 */
static void build_cipher_suite_list(private_tls_crypto_t *this)
{
	encryption_algorithm_t encr;
	integrity_algorithm_t mac;
	enumerator_t *encrs, *macs;
	tls_cipher_suite_t supported[64], unique[64];
	int count = 0, i, j;

	/* we assume that we support RSA, but no DHE yet */
	macs = lib->crypto->create_signer_enumerator(lib->crypto);
	while (macs->enumerate(macs, &mac))
	{
		switch (mac)
		{
			case AUTH_HMAC_SHA1_160:
				supported[count++] = TLS_RSA_WITH_NULL_SHA;
				break;
			case AUTH_HMAC_SHA2_256_256:
				supported[count++] = TLS_RSA_WITH_NULL_SHA256;
				break;
			case AUTH_HMAC_MD5_128:
				supported[count++] = TLS_RSA_WITH_NULL_MD5;
				break;
			default:
				break;
		}
		encrs = lib->crypto->create_crypter_enumerator(lib->crypto);
		while (encrs->enumerate(encrs, &encr))
		{
			switch (encr)
			{
				case ENCR_AES_CBC:
					switch (mac)
					{
						case AUTH_HMAC_SHA1_160:
							supported[count++] = TLS_RSA_WITH_AES_128_CBC_SHA;
							supported[count++] = TLS_RSA_WITH_AES_256_CBC_SHA;
							break;
						case AUTH_HMAC_SHA2_256_256:
							supported[count++] = TLS_RSA_WITH_AES_128_CBC_SHA256;
							supported[count++] = TLS_RSA_WITH_AES_128_CBC_SHA256;
							break;
						default:
							break;
					}
					break;
				case ENCR_3DES:
					switch (mac)
					{
						case AUTH_HMAC_SHA1_160:
							supported[count++] = TLS_RSA_WITH_3DES_EDE_CBC_SHA;
							break;
						default:
							break;
					}
					break;
				default:
					break;
			}
		}
		encrs->destroy(encrs);
	}
	macs->destroy(macs);

	/* remove duplicates */
	this->suite_count = 0;
	for (i = 0; i < count; i++)
	{
		bool match = FALSE;

		for (j = 0; j < this->suite_count; j++)
		{
			if (supported[i] == unique[j])
			{
				match = TRUE;
				break;
			}
		}
		if (!match)
		{
			unique[this->suite_count++] = supported[i];
		}
	}
	free(this->suites);
	this->suites = malloc(sizeof(tls_cipher_suite_t) * this->suite_count);
	memcpy(this->suites, unique, sizeof(tls_cipher_suite_t) * this->suite_count);
}

METHOD(tls_crypto_t, get_cipher_suites, int,
	private_tls_crypto_t *this, tls_cipher_suite_t **suites)
{
	*suites = this->suites;
	return this->suite_count;
}

/**
 * Create crypto primitives
 */
static bool create_ciphers(private_tls_crypto_t *this, tls_cipher_suite_t suite)
{
	suite_algs_t *algs;

	algs = find_suite(suite);
	if (!algs)
	{
		DBG1(DBG_IKE, "selected TLS suite not supported");
		return FALSE;
	}

	DESTROY_IF(this->prf);
	if (this->tls->get_version(this->tls) < TLS_1_2)
	{
		this->prf = tls_prf_create_10();
	}
	else
	{
		this->prf = tls_prf_create_12(algs->prf);
	}
	if (!this->prf)
	{
		DBG1(DBG_IKE, "selected TLS PRF not supported");
		return FALSE;
	}

	DESTROY_IF(this->signer_in);
	DESTROY_IF(this->signer_out);
	this->signer_in = lib->crypto->create_signer(lib->crypto, algs->mac);
	this->signer_out = lib->crypto->create_signer(lib->crypto, algs->mac);
	if (!this->signer_in || !this->signer_out)
	{
		DBG1(DBG_IKE, "selected TLS MAC %N not supported",
			 integrity_algorithm_names, algs->mac);
		return FALSE;
	}

	DESTROY_IF(this->crypter_in);
	DESTROY_IF(this->crypter_out);
	if (algs->encr == ENCR_NULL)
	{
		this->crypter_in = this->crypter_out = NULL;
	}
	else
	{
		this->crypter_in = lib->crypto->create_crypter(lib->crypto,
												algs->encr, algs->encr_size);
		this->crypter_out = lib->crypto->create_crypter(lib->crypto,
												algs->encr, algs->encr_size);
		if (!this->crypter_in || !this->crypter_out)
		{
			DBG1(DBG_IKE, "selected TLS crypter %N not supported",
				 encryption_algorithm_names, algs->encr);
			return FALSE;
		}
	}
	return TRUE;
}

METHOD(tls_crypto_t, select_cipher_suite, tls_cipher_suite_t,
	private_tls_crypto_t *this, tls_cipher_suite_t *suites, int count)
{
	int i, j;

	for (i = 0; i < this->suite_count; i++)
	{
		for (j = 0; j < count; j++)
		{
			if (this->suites[i] == suites[j])
			{
				if (create_ciphers(this, this->suites[i]))
				{
					this->suite = this->suites[i];
					return this->suite;
				}
			}
		}
	}
	return 0;
}

METHOD(tls_crypto_t, derive_master_secret, void,
	private_tls_crypto_t *this, chunk_t premaster,
	chunk_t client_random, chunk_t server_random)
{
	char master[48];
	chunk_t seed, block, client_write, server_write;
	int mks, eks = 0, ivs = 0;

	/* derive master secret */
	seed = chunk_cata("cc", client_random, server_random);
	this->prf->set_key(this->prf, premaster);
	this->prf->get_bytes(this->prf, "master secret", seed,
						 sizeof(master), master);

	this->prf->set_key(this->prf, chunk_from_thing(master));
	memset(master, 0, sizeof(master));

	/* derive key block for key expansion */
	mks = this->signer_out->get_key_size(this->signer_out);
	if (this->crypter_out)
	{
		eks = this->crypter_out->get_key_size(this->crypter_out);
		if (this->tls->get_version(this->tls) < TLS_1_2)
		{
			ivs = this->crypter_out->get_block_size(this->crypter_out);
		}
	}
	seed = chunk_cata("cc", server_random, client_random);
	block = chunk_alloca((mks + eks + ivs) * 2);
	this->prf->get_bytes(this->prf, "key expansion", seed, block.len, block.ptr);

	/* signer keys */
	client_write = chunk_create(block.ptr, mks);
	block = chunk_skip(block, mks);
	server_write = chunk_create(block.ptr, mks);
	block = chunk_skip(block, mks);
	if (this->tls->is_server(this->tls))
	{
		this->signer_in->set_key(this->signer_in, client_write);
		this->signer_out->set_key(this->signer_out, server_write);
	}
	else
	{
		this->signer_out->set_key(this->signer_out, client_write);
		this->signer_in->set_key(this->signer_in, server_write);
	}

	/* crypter keys, and IVs if < TLSv1.2 */
	if (this->crypter_out && this->crypter_in)
	{
		client_write = chunk_create(block.ptr, eks);
		block = chunk_skip(block, eks);
		server_write = chunk_create(block.ptr, eks);
		block = chunk_skip(block, eks);

		if (this->tls->is_server(this->tls))
		{
			this->crypter_in->set_key(this->crypter_in, client_write);
			this->crypter_out->set_key(this->crypter_out, server_write);
		}
		else
		{
			this->crypter_out->set_key(this->crypter_out, client_write);
			this->crypter_in->set_key(this->crypter_in, server_write);
		}
		if (ivs)
		{
			client_write = chunk_create(block.ptr, ivs);
			block = chunk_skip(block, ivs);
			server_write = chunk_create(block.ptr, ivs);
			block = chunk_skip(block, ivs);

			if (this->tls->is_server(this->tls))
			{
				this->iv_in = chunk_clone(client_write);
				this->iv_out = chunk_clone(server_write);
			}
			else
			{
				this->iv_out = chunk_clone(client_write);
				this->iv_in = chunk_clone(server_write);
			}
		}
	}
}

METHOD(tls_crypto_t, change_cipher, void,
	private_tls_crypto_t *this, bool inbound)
{
	if (inbound)
	{
		this->tls->change_cipher(this->tls, TRUE, this->signer_in,
								 this->crypter_in, this->iv_in);
	}
	else
	{
		this->tls->change_cipher(this->tls, FALSE, this->signer_out,
								 this->crypter_out, this->iv_out);
	}
}

METHOD(tls_crypto_t, get_prf, tls_prf_t*,
	private_tls_crypto_t *this)
{
	return this->prf;
}

METHOD(tls_crypto_t, destroy, void,
	private_tls_crypto_t *this)
{
	DESTROY_IF(this->signer_in);
	DESTROY_IF(this->signer_out);
	DESTROY_IF(this->crypter_in);
	DESTROY_IF(this->crypter_out);
	free(this->iv_in.ptr);
	free(this->iv_out.ptr);
	DESTROY_IF(this->prf);
	free(this->suites);
	free(this);
}

/**
 * See header
 */
tls_crypto_t *tls_crypto_create(tls_t *tls)
{
	private_tls_crypto_t *this;

	INIT(this,
		.public = {
			.get_cipher_suites = _get_cipher_suites,
			.select_cipher_suite = _select_cipher_suite,
			.derive_master_secret = _derive_master_secret,
			.change_cipher = _change_cipher,
			.get_prf = _get_prf,
			.destroy = _destroy,
		},
		.tls = tls,
	);

	build_cipher_suite_list(this);

	return &this->public;
}
