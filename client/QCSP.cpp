/*
 * QDigiDoc4
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include "QCSP.h"

#include <common/PinDialog.h>
#include <common/TokenData.h>

#include <QtCore/QDebug>
#include <QtWidgets/QApplication>

#include <WinCrypt.h>

#include <openssl/obj_mac.h>

class QCSP::Private
{
public:
	QCSP::PinStatus error = QCSP::PinOK;
	PCCERT_CONTEXT cert = nullptr;
};



QCSP::QCSP(QObject *parent)
	: QWin(parent)
	, d(new Private)
{
}

QCSP::~QCSP()
{
	if(d->cert)
		CertFreeCertificateContext(d->cert);
	delete d;
}

QByteArray QCSP::decrypt( const QByteArray &data )
{
	d->error = PinOK;
	if(!d->cert)
		return QByteArray();

	DWORD flags = CRYPT_ACQUIRE_PREFER_NCRYPT_KEY_FLAG|CRYPT_ACQUIRE_COMPARE_KEY_FLAG;
	HCRYPTPROV_OR_NCRYPT_KEY_HANDLE key = 0;
	DWORD spec = 0;
	BOOL freeKey = false;
	CryptAcquireCertificatePrivateKey(d->cert, flags, nullptr, &key, &spec, &freeKey);
	qDebug() << "Key spec" << spec;
	if(!key)
		return QByteArray();

	DWORD size = DWORD(data.size());
	QByteArray result(int(size), 0);
	SECURITY_STATUS err = 0;
	switch(spec)
	{
	case CERT_NCRYPT_KEY_SPEC:
	{
		err = NCryptDecrypt(key, PBYTE(data.constData()), DWORD(data.size()), nullptr,
			PBYTE(result.data()), DWORD(result.size()), &size, NCRYPT_PAD_PKCS1_FLAG);
		if(freeKey)
			NCryptFreeObject(key);
		break;
	}
	case AT_KEYEXCHANGE:
	{
		result = data;
		std::reverse(result.begin(), result.end());
		if(!CryptDecrypt(key, 0, true, 0, LPBYTE(result.data()), &size))
			err = GetLastError();
		if(freeKey)
			CryptReleaseContext(key, 0);
		break;
	}
	case AT_SIGNATURE:
	default:
		size = 0;
		if(freeKey)
			CryptReleaseContext(key, 0);
		break;
	}

	switch(err)
	{
	case ERROR_SUCCESS:
		result.resize(size);
		return result;
	case SCARD_W_CANCELLED_BY_USER:
	case ERROR_CANCELLED:
		d->error = PinCanceled;
	default:
		return QByteArray();
	}
}

QByteArray QCSP::deriveConcatKDF(const QByteArray &publicKey, const QString &digest, int keySize,
	const QByteArray &algorithmID, const QByteArray &partyUInfo, const QByteArray &partyVInfo) const
{
	d->error = PinUnknown;
	QByteArray derived;
	DWORD flags = CRYPT_ACQUIRE_ONLY_NCRYPT_KEY_FLAG|CRYPT_ACQUIRE_COMPARE_KEY_FLAG;
	HCRYPTPROV_OR_NCRYPT_KEY_HANDLE key = 0;
	DWORD spec = 0;
	BOOL freeKey = false;
	CryptAcquireCertificatePrivateKey(d->cert, flags, nullptr, &key, &spec, &freeKey);
	qDebug() << "Key spec" << spec;
	if(!key)
		return derived;
	NCRYPT_PROV_HANDLE prov = 0;
	DWORD size = 0;
	if(NCryptGetProperty(key, NCRYPT_PROVIDER_HANDLE_PROPERTY, PBYTE(&prov), sizeof(prov), &size, 0))
	{
		if(freeKey)
			NCryptFreeObject(key);
		return derived;
	}
	int err = derive(prov, key, publicKey, digest, keySize, algorithmID, partyUInfo, partyVInfo, derived);
	if(freeKey)
		NCryptFreeObject(key);
	switch(err)
	{
	case ERROR_SUCCESS:
		d->error = PinOK;
		return derived;
	case SCARD_W_CANCELLED_BY_USER:
		d->error = PinCanceled;
	default:
		return derived;
	}
}

QCSP::PinStatus QCSP::lastError() const { return d->error; }

QList<TokenData> QCSP::tokens() const
{
	qWarning() << "Start enumerationg certs";
	QList<TokenData> certs;
	PCCERT_CONTEXT find = nullptr;
	HCERTSTORE s = CertOpenStore(CERT_STORE_PROV_SYSTEM_W,
		X509_ASN_ENCODING, 0, CERT_SYSTEM_STORE_CURRENT_USER | CERT_STORE_READONLY_FLAG, L"MY");
	while(find = CertFindCertificateInStore(s, X509_ASN_ENCODING|PKCS_7_ASN_ENCODING, 0, CERT_FIND_ANY, nullptr, find))
	{
		DWORD flags = CRYPT_ACQUIRE_PREFER_NCRYPT_KEY_FLAG|CRYPT_ACQUIRE_COMPARE_KEY_FLAG|CRYPT_ACQUIRE_SILENT_FLAG;
		HCRYPTPROV_OR_NCRYPT_KEY_HANDLE key = 0;
		DWORD spec = 0;
		BOOL freeKey = false;
		CryptAcquireCertificatePrivateKey(find, flags, nullptr, &key, &spec, &freeKey);
		SslCertificate cert(QByteArray::fromRawData((const char*)find->pbCertEncoded, int(find->cbCertEncoded)), QSsl::Der);
		qDebug() << cert.subjectInfo("CN") << "has key" << key;
		if(!key)
			continue;
		if(freeKey)
		{
			switch(spec)
			{
			case CERT_NCRYPT_KEY_SPEC: NCryptFreeObject(key); break;
			case AT_SIGNATURE:
			case AT_KEYEXCHANGE:
			default: CryptReleaseContext(key, 0); break;
			}
		}
		TokenData t;
		t.setCard(cert.subjectInfo("CN"));
		t.setCert(cert);
		certs << t;
	}
	CertCloseStore(s, 0);
	qWarning() << "End enumerationg certs";
	return certs;
}

TokenData QCSP::selectCert(const SslCertificate &cert)
{
	TokenData t;
	t.setCard(cert.subjectInfo("CN"));
	t.setCert(cert);

	if(d->cert)
		CertFreeCertificateContext(d->cert);
	d->cert = nullptr;

	QByteArray der = cert.toDer();
	PCCERT_CONTEXT tmp = CertCreateCertificateContext(X509_ASN_ENCODING, PBYTE(der.constData()), DWORD(der.size()));
	if(!tmp)
		return t;

	HCERTSTORE s = CertOpenStore(CERT_STORE_PROV_SYSTEM_W,
		X509_ASN_ENCODING, 0, CERT_SYSTEM_STORE_CURRENT_USER | CERT_STORE_READONLY_FLAG, L"MY");
	d->cert = CertFindCertificateInStore(s, X509_ASN_ENCODING, 0, CERT_FIND_EXISTING, tmp, nullptr);
	CertCloseStore(s, 0);
	CertFreeCertificateContext(tmp);
	qDebug() << "Selected cert" << cert.subjectInfo("CN");
	return t;
}

QByteArray QCSP::sign(int method, const QByteArray &digest) const
{
	QByteArray result;
	d->error = PinOK;
	if(!d->cert)
		return result;

	BCRYPT_PKCS1_PADDING_INFO padInfo = { NCRYPT_SHA256_ALGORITHM };
	ALG_ID alg = CALG_SHA_256;
	switch(method)
	{
	case NID_sha224:
		padInfo.pszAlgId = L"SHA224";
		break;
	case NID_sha256:
		padInfo.pszAlgId = NCRYPT_SHA256_ALGORITHM;
		alg = CALG_SHA_256;
		break;
	case NID_sha384:
		padInfo.pszAlgId = NCRYPT_SHA384_ALGORITHM;
		alg = CALG_SHA_384;
		break;
	case NID_sha512:
		padInfo.pszAlgId = NCRYPT_SHA512_ALGORITHM;
		alg = CALG_SHA_512;
		break;
	default: break;
	}

	DWORD flags = CRYPT_ACQUIRE_PREFER_NCRYPT_KEY_FLAG|CRYPT_ACQUIRE_COMPARE_KEY_FLAG;
	HCRYPTPROV_OR_NCRYPT_KEY_HANDLE key = 0;
	DWORD spec = 0;
	BOOL freeKey = false;
	CryptAcquireCertificatePrivateKey(d->cert, flags, nullptr, &key, &spec, &freeKey);
	qDebug() << "Key spec" << spec;
	if(!key)
		return result;

	SECURITY_STATUS err = 0;
	switch( spec )
	{
	case CERT_NCRYPT_KEY_SPEC:
	{
		DWORD size = 0;
		QString algo(5, 0);
		err = NCryptGetProperty(key, NCRYPT_ALGORITHM_GROUP_PROPERTY, PBYTE(algo.data()), (algo.size() + 1) * 2, &size, 0);
		algo.resize(size/2 - 1);
		bool isRSA = algo == "RSA";

		err = NCryptSignHash(key, isRSA ? &padInfo : nullptr, PBYTE(digest.constData()), DWORD(digest.size()),
			nullptr, 0, &size, isRSA ? BCRYPT_PAD_PKCS1 : 0);
		if(FAILED(err))
		{
			if(freeKey)
				NCryptFreeObject(key);
			return result;
		}
		result.resize(int(size));
		err = NCryptSignHash(key, isRSA ? &padInfo : nullptr, PBYTE(digest.constData()), DWORD(digest.size()),
			PBYTE(result.data()), DWORD(result.size()), &size, isRSA ? BCRYPT_PAD_PKCS1 : 0);
		if( freeKey )
			NCryptFreeObject(key);
		break;
	}
	case AT_KEYEXCHANGE:
	case AT_SIGNATURE:
	{
		if(method == NID_sha224)
		{
			if(freeKey)
				CryptReleaseContext(key, 0);
			return result;
		}

		HCRYPTHASH hash = 0;
		if(!CryptCreateHash(key, alg, 0, 0, &hash))
		{
			if(freeKey)
				CryptReleaseContext(key, 0);
			return result;
		}

		if(!CryptSetHashParam(hash, HP_HASHVAL, LPBYTE(digest.constData()), 0))
		{
			CryptDestroyHash(hash);
			if(freeKey)
				CryptReleaseContext(key, 0);
			return result;
		}
		DWORD size = 0;
		if(!CryptSignHashW(hash, spec, nullptr, 0, nullptr, &size))
			err = GetLastError();
		result.resize(int(size));
		if(!CryptSignHashW(hash, spec, nullptr, 0, LPBYTE(result.data()), &size))
			err = GetLastError();
		std::reverse(result.begin(), result.end());

		if(freeKey)
			CryptReleaseContext(key, 0);
		CryptDestroyHash(hash);
		break;
	}
	default: break;
	}

	switch(err)
	{
	case ERROR_SUCCESS:
		d->error = PinOK;
		return result;
	case SCARD_W_CANCELLED_BY_USER:
	case ERROR_CANCELLED:
		d->error = PinCanceled;
	default:
		result.clear();
		return result;
	}
}
