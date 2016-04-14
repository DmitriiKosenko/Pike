#pike __REAL_VERSION__
#pragma strict_types
#require constant(Nettle.SHA512)

//! SHA512 is another hash function specified by NIST, intended as a
//! replacement for @[SHA1], generating larger digests. It outputs hash
//! values of 512 bits, or 64 octets.

inherit Nettle.SHA512;

Standards.ASN1.Types.Identifier pkcs_hash_id()
{
  return Standards.PKCS.Identifiers.sha512_id;
}

//! @module HMAC

//! @ignore
protected class _HMAC
{
//! @endignore

  inherit ::this_program;

  //! JWS algorithm identifier.
  //!
  //! @seealso
  //!   @rfc{7518:3.1@}
  constant jwa = "HS512";

//! @ignore
}
//! @endignore

//! @endmodule HMAC
