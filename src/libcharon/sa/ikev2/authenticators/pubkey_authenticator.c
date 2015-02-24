/*
 * Copyright (C) 2008-2015 Tobias Brunner
 * Copyright (C) 2005-2009 Martin Willi
 * Copyright (C) 2005 Jan Hutter
 * Hochschule fuer Technik Rapperswil
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

#include "pubkey_authenticator.h"

#include <daemon.h>
#include <encoding/payloads/auth_payload.h>
#include <sa/ikev2/keymat_v2.h>
#include <asn1/asn1.h>
#include <asn1/oid.h>

typedef struct private_pubkey_authenticator_t private_pubkey_authenticator_t;

/**
 * Private data of an pubkey_authenticator_t object.
 */
struct private_pubkey_authenticator_t {

	/**
	 * Public authenticator_t interface.
	 */
	pubkey_authenticator_t public;

	/**
	 * Assigned IKE_SA
	 */
	ike_sa_t *ike_sa;

	/**
	 * nonce to include in AUTH calculation
	 */
	chunk_t nonce;

	/**
	 * IKE_SA_INIT message data to include in AUTH calculation
	 */
	chunk_t ike_sa_init;

	/**
	 * Reserved bytes of ID payload
	 */
	char reserved[3];
};

/**
 * Parse authentication data used for Signature Authentication as per RFC 7427
 */
static bool parse_signature_auth_data(chunk_t *auth_data, key_type_t *key_type,
									  signature_scheme_t *scheme)
{
	u_int8_t len;
	int oid;

	if (!auth_data->len)
	{
		return FALSE;
	}
	len = auth_data->ptr[0];
	*auth_data = chunk_skip(*auth_data, 1);
	/* we currently don't support schemes that require parameters */
	oid = asn1_parse_algorithmIdentifier(*auth_data, 1, NULL);
	*scheme = signature_scheme_from_oid(oid);
	if (*scheme == SIGN_UNKNOWN)
	{
		return FALSE;
	}
	*key_type = key_type_from_signature_scheme(*scheme);
	*auth_data = chunk_skip(*auth_data, len);
	return TRUE;
}

/**
 * Build authentication data used for Signature Authentication as per RFC 7427
 */
static bool build_signature_auth_data(chunk_t *auth_data,
									  signature_scheme_t scheme)
{
	chunk_t data;
	u_int8_t len;
	int oid;

	oid = signature_scheme_to_oid(scheme);
	if (oid == OID_UNKNOWN)
	{
		return FALSE;
	}
	data = asn1_algorithmIdentifier(oid);
	len = data.len;
	*auth_data = chunk_cat("cmm", chunk_from_thing(len), data, *auth_data);
	return TRUE;
}

/**
 * Select a signature scheme based on our configuration, the other peer's
 * capabilities and the private key
 */
static signature_scheme_t select_signature_scheme(keymat_v2_t *keymat,
									auth_cfg_t *auth, private_key_t *private)
{
	enumerator_t *enumerator;
	signature_scheme_t scheme = SIGN_UNKNOWN;
	hash_algorithm_t hash;
	char *plugin_name;
	uintptr_t config;
	auth_rule_t rule;
	key_type_t key_type;
	bool have_config = FALSE;
	int oid;

	key_type = private->get_type(private);
	enumerator = auth->create_enumerator(auth);
	while (enumerator->enumerate(enumerator, &rule, &config))
	{
		if (rule != AUTH_RULE_SIGNATURE_SCHEME)
		{
			continue;
		}
		have_config = TRUE;
		if (key_type == key_type_from_signature_scheme(config) &&
			keymat->hash_algorithm_supported(keymat,
										hasher_from_signature_scheme(config)))
		{
			scheme = config;
			break;
		}
	}
	enumerator->destroy(enumerator);

	if (scheme == SIGN_UNKNOWN && !have_config)
	{
		/* if no specific configuration, find a scheme supported by us, the
		 * other peer and the key */
		enumerator = lib->crypto->create_hasher_enumerator(lib->crypto);
		while (enumerator->enumerate(enumerator, &hash, &plugin_name))
		{
			if (keymat->hash_algorithm_supported(keymat, hash))
			{
				oid = hasher_signature_algorithm_to_oid(hash, key_type);
				if (oid != OID_UNKNOWN)
				{
					scheme = signature_scheme_from_oid(oid);
					break;
				}
			}
		}
		enumerator->destroy(enumerator);
	}
	return scheme;
}

METHOD(authenticator_t, build, status_t,
	private_pubkey_authenticator_t *this, message_t *message)
{
	chunk_t octets = chunk_empty, auth_data;
	status_t status = FAILED;
	private_key_t *private;
	identification_t *id;
	auth_cfg_t *auth;
	auth_payload_t *auth_payload;
	auth_method_t auth_method;
	signature_scheme_t scheme = SIGN_UNKNOWN;
	keymat_v2_t *keymat;

	id = this->ike_sa->get_my_id(this->ike_sa);
	auth = this->ike_sa->get_auth_cfg(this->ike_sa, TRUE);
	private = lib->credmgr->get_private(lib->credmgr, KEY_ANY, id, auth);
	if (private == NULL)
	{
		DBG1(DBG_IKE, "no private key found for '%Y'", id);
		return NOT_FOUND;
	}
	keymat = (keymat_v2_t*)this->ike_sa->get_keymat(this->ike_sa);

	if (this->ike_sa->supports_extension(this->ike_sa, EXT_SIGNATURE_AUTH))
	{
		scheme = select_signature_scheme(keymat, auth, private);
		auth_method = AUTH_DS;
		if (scheme == SIGN_UNKNOWN)
		{
			DBG1(DBG_IKE, "unable to determine signature scheme for %N key",
				 key_type_names, private->get_type(private));
			return status;
		}
	}
	else
	{
		switch (private->get_type(private))
		{
			case KEY_RSA:
				scheme = SIGN_RSA_EMSA_PKCS1_SHA1;
				auth_method = AUTH_RSA;
				break;
			case KEY_ECDSA:
				/* deduct the signature scheme from the keysize */
				switch (private->get_keysize(private))
				{
					case 256:
						scheme = SIGN_ECDSA_256;
						auth_method = AUTH_ECDSA_256;
						break;
					case 384:
						scheme = SIGN_ECDSA_384;
						auth_method = AUTH_ECDSA_384;
						break;
					case 521:
						scheme = SIGN_ECDSA_521;
						auth_method = AUTH_ECDSA_521;
						break;
					default:
						DBG1(DBG_IKE, "%d bit ECDSA private key size not "
							 "supported", private->get_keysize(private));
						return status;
				}
				break;
			default:
				DBG1(DBG_IKE, "private key of type %N not supported",
					 key_type_names, private->get_type(private));
				return status;
		}
	}

	if (keymat->get_auth_octets(keymat, FALSE, this->ike_sa_init,
								this->nonce, id, this->reserved, &octets) &&
		private->sign(private, scheme, octets, &auth_data))
	{
		if (auth_method != AUTH_DS ||
			build_signature_auth_data(&auth_data, scheme))
		{
			auth_payload = auth_payload_create();
			auth_payload->set_auth_method(auth_payload, auth_method);
			auth_payload->set_data(auth_payload, auth_data);
			chunk_free(&auth_data);
			message->add_payload(message, (payload_t*)auth_payload);
			status = SUCCESS;
		}
	}
	DBG1(DBG_IKE, "authentication of '%Y' (myself) with %N %s", id,
		 auth_method_names, auth_method,
		 (status == SUCCESS)? "successful":"failed");
	chunk_free(&octets);
	private->destroy(private);

	return status;
}

METHOD(authenticator_t, process, status_t,
	private_pubkey_authenticator_t *this, message_t *message)
{
	public_key_t *public;
	auth_method_t auth_method;
	auth_payload_t *auth_payload;
	chunk_t auth_data, octets;
	identification_t *id;
	auth_cfg_t *auth, *current_auth;
	enumerator_t *enumerator;
	key_type_t key_type = KEY_ECDSA;
	signature_scheme_t scheme;
	status_t status = NOT_FOUND;
	keymat_v2_t *keymat;

	auth_payload = (auth_payload_t*)message->get_payload(message, PLV2_AUTH);
	if (!auth_payload)
	{
		return FAILED;
	}
	auth_method = auth_payload->get_auth_method(auth_payload);
	auth_data = auth_payload->get_data(auth_payload);
	switch (auth_method)
	{
		case AUTH_RSA:
			key_type = KEY_RSA;
			scheme = SIGN_RSA_EMSA_PKCS1_SHA1;
			break;
		case AUTH_ECDSA_256:
			scheme = SIGN_ECDSA_256;
			break;
		case AUTH_ECDSA_384:
			scheme = SIGN_ECDSA_384;
			break;
		case AUTH_ECDSA_521:
			scheme = SIGN_ECDSA_521;
			break;
		case AUTH_DS:
			if (parse_signature_auth_data(&auth_data, &key_type, &scheme))
			{
				break;
			}
			/* fall-through */
		default:
			return INVALID_ARG;
	}
	id = this->ike_sa->get_other_id(this->ike_sa);
	keymat = (keymat_v2_t*)this->ike_sa->get_keymat(this->ike_sa);
	if (!keymat->get_auth_octets(keymat, TRUE, this->ike_sa_init,
								 this->nonce, id, this->reserved, &octets))
	{
		return FAILED;
	}
	auth = this->ike_sa->get_auth_cfg(this->ike_sa, FALSE);
	enumerator = lib->credmgr->create_public_enumerator(lib->credmgr,
														key_type, id, auth);
	while (enumerator->enumerate(enumerator, &public, &current_auth))
	{
		if (public->verify(public, scheme, octets, auth_data))
		{
			DBG1(DBG_IKE, "authentication of '%Y' with %N successful",
						   id, auth_method_names, auth_method);
			status = SUCCESS;
			auth->merge(auth, current_auth, FALSE);
			auth->add(auth, AUTH_RULE_AUTH_CLASS, AUTH_CLASS_PUBKEY);
			break;
		}
		else
		{
			status = FAILED;
			DBG1(DBG_IKE, "signature validation failed, looking for another key");
		}
	}
	enumerator->destroy(enumerator);
	chunk_free(&octets);
	if (status == NOT_FOUND)
	{
		DBG1(DBG_IKE, "no trusted %N public key found for '%Y'",
			 key_type_names, key_type, id);
	}
	return status;
}

METHOD(authenticator_t, destroy, void,
	private_pubkey_authenticator_t *this)
{
	free(this);
}

/*
 * Described in header.
 */
pubkey_authenticator_t *pubkey_authenticator_create_builder(ike_sa_t *ike_sa,
									chunk_t received_nonce, chunk_t sent_init,
									char reserved[3])
{
	private_pubkey_authenticator_t *this;

	INIT(this,
		.public = {
			.authenticator = {
				.build = _build,
				.process = (void*)return_failed,
				.is_mutual = (void*)return_false,
				.destroy = _destroy,
			},
		},
		.ike_sa = ike_sa,
		.ike_sa_init = sent_init,
		.nonce = received_nonce,
	);
	memcpy(this->reserved, reserved, sizeof(this->reserved));

	return &this->public;
}

/*
 * Described in header.
 */
pubkey_authenticator_t *pubkey_authenticator_create_verifier(ike_sa_t *ike_sa,
									chunk_t sent_nonce, chunk_t received_init,
									char reserved[3])
{
	private_pubkey_authenticator_t *this;

	INIT(this,
		.public = {
			.authenticator = {
				.build = (void*)return_failed,
				.process = _process,
				.is_mutual = (void*)return_false,
				.destroy = _destroy,
			},
		},
		.ike_sa = ike_sa,
		.ike_sa_init = received_init,
		.nonce = sent_nonce,
	);
	memcpy(this->reserved, reserved, sizeof(this->reserved));

	return &this->public;
}
