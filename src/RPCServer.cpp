
#include <fstream>
#include <iostream>

#include <boost/bind.hpp>
#include <boost/foreach.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string.hpp>

#include <openssl/md5.h>

#include "../json/reader.h"
#include "../json/writer.h"

#include "RPCServer.h"
#include "RequestParser.h"
#include "HttpReply.h"
#include "HttpsClient.h"
#include "Application.h"
#include "RPC.h"
#include "Wallet.h"
#include "Conversion.h"
#include "NewcoinAddress.h"
#include "AccountState.h"
#include "utils.h"

#define VALIDATORS_FETCH_SECONDS	30
#define VALIDATORS_FILE_PATH		"/" VALIDATORS_FILE_NAME
#define VALIDATORS_FILE_BYTES_MAX	(50 << 10)

/*
Just read from wire until the entire request is in.
*/

RPCServer::RPCServer(boost::asio::io_service& io_service , NetworkOPs* nopNetwork)
	: mNetOps(nopNetwork), mSocket(io_service)
{
}

void RPCServer::connected()
{
	//BOOST_LOG_TRIVIAL(info) << "RPC request";
	std::cout << "RPC request" << std::endl;

	mSocket.async_read_some(boost::asio::buffer(mReadBuffer),
		boost::bind(&RPCServer::handle_read, shared_from_this(),
		boost::asio::placeholders::error,
		boost::asio::placeholders::bytes_transferred));
}

void RPCServer::handle_read(const boost::system::error_code& e,
	std::size_t bytes_transferred)
{
	if (!e)
	{
		boost::tribool result;
		result = mRequestParser.parse(
			mIncomingRequest, mReadBuffer.data(), mReadBuffer.data() + bytes_transferred);

		if (result)
		{
			mReplyStr=handleRequest(mIncomingRequest.mBody);
			sendReply();
		}
		else if (!result)
		{ // bad request
			std::cout << "bad request" << std::endl;
		}
		else
		{  // not done keep reading
			mSocket.async_read_some(boost::asio::buffer(mReadBuffer),
				boost::bind(&RPCServer::handle_read, shared_from_this(),
				boost::asio::placeholders::error,
				boost::asio::placeholders::bytes_transferred));
		}
	}
	else if (e != boost::asio::error::operation_aborted)
	{

	}
}

std::string RPCServer::handleRequest(const std::string& requestStr)
{
	std::cout << "handleRequest " << requestStr << std::endl;
	Json::Value id;

	// Parse request
	Json::Value valRequest;
	Json::Reader reader;
	if (!reader.parse(requestStr, valRequest) || valRequest.isNull() || !valRequest.isObject())
		return(HTTPReply(400, ""));

	// Parse id now so errors from here on will have the id
	id = valRequest["id"];

	// Parse method
	Json::Value valMethod = valRequest["method"];
	if (valMethod.isNull())
		return(HTTPReply(400, ""));
	if (!valMethod.isString())
		return(HTTPReply(400, ""));
	std::string strMethod = valMethod.asString();

	// Parse params
	Json::Value valParams = valRequest["params"];
	if (valParams.isNull())
		valParams = Json::Value(Json::arrayValue);
	else if (!valParams.isArray())
		return(HTTPReply(400, ""));

#ifdef DEBUG
	Json::StyledStreamWriter w;
	w.write(std::cerr, valParams);
#endif

	Json::Value result(doCommand(strMethod, valParams));

#ifdef DEBUG
	w.write(std::cerr, result);
#endif

	std::string strReply = JSONRPCReply(result, Json::Value(), id);
	return( HTTPReply(200, strReply) );
}

int RPCServer::getParamCount(const Json::Value& params)
{ // If non-array, only counts strings
	if (params.isNull()) return 0;
	if (params.isArray()) return params.size();
	if (!params.isConvertibleTo(Json::stringValue))
		return 0;
	return 1;
}

#if 0
// now, expire, n
bool RPCServer::parseAcceptRate(const std::string& sAcceptRate)
{
	if (!sAcceptRate.compare("expire"))
		0;

	return true;
}
#endif

bool RPCServer::extractString(std::string& param, const Json::Value& params, int index)
{
	if (params.isNull()) return false;

	if (index!=0)
	{
		if (!params.isArray() || !params.isValidIndex(index))
			return false;
		Json::Value p(params.get(index, Json::nullValue));
		if (p.isNull() || !p.isConvertibleTo(Json::stringValue))
			return false;
		param = p.asString();
		return true;
	}

	if (params.isArray())
	{
		if ( (!params.isValidIndex(0)) || (!params[0u].isConvertibleTo(Json::stringValue)) )
			return false;
		param = params[0u].asString();
		return true;
	}

	if (!params.isConvertibleTo(Json::stringValue))
		return false;
	param = params.asString();
	return true;
}

// Get the generator for a regular seed so we may index source accounts from the master public generator.
Json::Value RPCServer::getGenerator(const uint256& uLedger, const NewcoinAddress& naSeed, NewcoinAddress& naMasterGenerator)
{
	NewcoinAddress		naGenerator;
	NewcoinAddress		na0Public;		// To find the generator index.
	NewcoinAddress		na0Private;		// To decrypt the master generator's cipher.

	naGenerator.setFamilyGenerator(naSeed);
	na0Public.setAccountPublic(naGenerator, 0);
	na0Private.setAccountPrivate(naGenerator, naSeed, 0);

	SLE::pointer		sleGen			= mNetOps->getGenerator(uLedger, na0Public.getAccountID());

	if (!sleGen)
	{
		// No account has been claimed or has had it password set for seed.
		return JSONRPCError(500, "wrong password");
	}

	std::vector<unsigned char>	vucCipher			= sleGen->getIFieldVL(sfGenerator);
	std::vector<unsigned char>	vucMasterGenerator	= na0Private.accountPrivateDecrypt(na0Public, vucCipher);
	if (vucMasterGenerator.empty())
	{
		return JSONRPCError(500, "internal error: password failed to decrypt master public generator");
	}

	naMasterGenerator.setFamilyGenerator(vucMasterGenerator);

	return Json::Value(Json::objectValue);
}

// Given a seed and a source account get the public and private key for authorizing transactions for the account.
Json::Value RPCServer::authorize(const uint256& uLedger,
	const NewcoinAddress& naSeed, const NewcoinAddress& naSrcAccountID,
	NewcoinAddress& naAccountPublic, NewcoinAddress& naAccountPrivate,
	AccountState::pointer& asSrc,
	const NewcoinAddress& naVerifyGenerator)
{
	asSrc	= mNetOps->getAccountState(uLedger, naSrcAccountID);
	if (!asSrc)
	{
		return JSONRPCError(500, "source account does not exist");
	}

	if (!asSrc->bHaveAuthorizedKey())
	{
		return JSONRPCError(500, "source account has not been claimed");
	}

	NewcoinAddress	naMasterGenerator;

	Json::Value	obj	= getGenerator(uLedger, naSeed, naMasterGenerator);

	if (!obj.empty())
		return obj;

	if (naVerifyGenerator.isValid() && naMasterGenerator != naVerifyGenerator)
	{
		std::cerr << "naAccountPublic: wrong seed" << std::endl;

		return JSONRPCError(500, "wrong seed");
	}

	//
	// Find the index of the account from the master generator, so we can generate the public and private keys.
	//
	NewcoinAddress		naMasterAccountPublic;
	uint				iIndex = -1;	// Compensate for initial increment.

	// XXX Stop after Config.account_probe_max
	// Don't look at ledger entries to determine if the account exists.  Don't want to leak to thin server that these accounts are
	// related.
	do {
		++iIndex;
		naMasterAccountPublic.setAccountPublic(naMasterGenerator, iIndex);
	} while (naSrcAccountID.getAccountID() != naMasterAccountPublic.getAccountID());

	// Use the regular generator to determine the associated public and private keys.
	NewcoinAddress		naGenerator;

	naGenerator.setFamilyGenerator(naSeed);
	naAccountPublic.setAccountPublic(naGenerator, iIndex);
	naAccountPrivate.setAccountPrivate(naGenerator, naSeed, iIndex);

	if (asSrc->getAuthorizedKey().getAccountID() != naAccountPublic.getAccountID())
	{
		std::cerr << "iIndex: " << iIndex << std::endl;
		std::cerr << "sfAuthorizedKey: " << strHex(asSrc->getAuthorizedKey().getAccountID()) << std::endl;
		std::cerr << "naAccountPublic: " << strHex(naAccountPublic.getAccountID()) << std::endl;

		return JSONRPCError(500, "wrong password (changed)");
	}

	return obj;
}

// <-- bIndex: true if iIndex > 0 and used the index.
Json::Value RPCServer::accountFromString(const uint256& uLedger, NewcoinAddress& naAccount, bool& bIndex, const std::string& strIdent, const int iIndex)
{
	NewcoinAddress	naSeed;

	if (naAccount.setAccountPublic(strIdent) || naAccount.setAccountID(strIdent))
	{
		// Got the account.
		bIndex	= false;
	}
	// Must be a seed.
	else if (!naSeed.setFamilySeedGeneric(strIdent))
	{
		return "disallowed seed";
	}
	else
	{
		// We allow the use of the seeds to access #0.
		// This is poor practice and merely for debuging convenience.
		NewcoinAddress		naGenerator;
		NewcoinAddress		naRegular0Public;
		NewcoinAddress		naRegular0Private;

		naGenerator.setFamilyGenerator(naSeed);

		naRegular0Public.setAccountPublic(naGenerator, 0);
		naRegular0Private.setAccountPrivate(naGenerator, naSeed, 0);

		uint160				uGeneratorID	= naRegular0Public.getAccountID();
		SLE::pointer		sleGen			= mNetOps->getGenerator(uLedger, naRegular0Public.getAccountID());
		if (!sleGen)
		{
			// Didn't find a generator map, assume it is a master generator.
			nothing();
		}
		else
		{
			// Found master public key.
			std::vector<unsigned char>	vucCipher				= sleGen->getIFieldVL(sfGenerator);
			std::vector<unsigned char>	vucMasterGenerator		= naRegular0Private.accountPrivateDecrypt(naRegular0Public, vucCipher);
			if (vucMasterGenerator.empty())
			{
				return "internal error: password failed to decrypt master public generator";
			}

			naGenerator.setFamilyGenerator(vucMasterGenerator);
		}

		bIndex	= !iIndex;

		naAccount.setAccountPublic(naGenerator, iIndex);
	}

	return Json::Value(Json::objectValue);
}

// account_email_set <seed> <paying_account> [<email_address>]
Json::Value RPCServer::doAccountEmailSet(Json::Value &params)
{
	if (params.size() < 2 || params.size() > 3)
	{
		return "invalid params";
	}
	else if (!mNetOps->available())
	{
		return JSONRPCError(503, "network not available");
	}

	// Hash as per: http://en.gravatar.com/site/implement/hash/
	std::string					strEmail	= 3 == params.size() ? params[2u].asString() : "";
	std::vector<unsigned char>	vucMD5(128/8, 0);
	std::string					strMD5Lower;

	boost::trim(strEmail);
	boost::to_lower(strEmail);

	MD5(reinterpret_cast<const unsigned char*>(strEmail.c_str()), strEmail.size(), &vucMD5.front());

	strMD5Lower	= strHex(vucMD5);
	boost::to_lower(strMD5Lower);

	Json::Value					ret(Json::objectValue);

	ret["email"]		= strEmail;
	ret["hash"]			= strHex(vucMD5);
	ret["url_gravatar"]	= str(boost::format("http://www.gravatar.com/avatar/%s") % strMD5Lower);

	return ret;
}

// account_info <account>|<nickname>|<account_public_key>
// account_info <seed>|<pass_phrase>|<key> [<index>]
Json::Value RPCServer::doAccountInfo(Json::Value &params)
{
	if (params.size() < 1 || params.size() > 2)
	{
		return "invalid params";
	}
	else if (!mNetOps->available())
	{
		return JSONRPCError(503, "network not available");
	}
	else
	{
		std::string		strIdent	= params[0u].asString();
		bool			bIndex;
		int				iIndex		= 2 == params.size()? boost::lexical_cast<int>(params[1u].asString()) : 0;
		NewcoinAddress	naAccount;

		Json::Value ret;

		uint256			uLedger		= mNetOps->getClosedLedger();

		ret	= accountFromString(uLedger, naAccount, bIndex, strIdent, iIndex);

		if (!ret.empty())
			return ret;

		// Get info on account.
		ret	= Json::Value(Json::objectValue);

		AccountState::pointer as	= mNetOps->getAccountState(uLedger, naAccount);
		if (as)
		{
			as->addJson(ret);
		}
		else
		{
			ret["account"]	= naAccount.humanAccountID();
			ret["status"]	= "NotFound";
			if (bIndex)
				ret["index"]	= iIndex;
		}

		return ret;
	}
}

// account_lines <account>|<nickname>|<account_public_key> [<index>]
Json::Value RPCServer::doAccountLines(Json::Value &params)
{
	if (params.size() < 1 || params.size() > 2)
	{
		return "invalid params";
	}
	else if (!mNetOps->available())
	{
		return JSONRPCError(503, "network not available");
	}
	else
	{
		std::string		strIdent	= params[0u].asString();
		bool			bIndex;
		int				iIndex		= 2 == params.size()? boost::lexical_cast<int>(params[1u].asString()) : 0;
		uint256			uLedger		= mNetOps->getClosedLedger();

		NewcoinAddress	naAccount;

		Json::Value ret;

		ret	= accountFromString(uLedger, naAccount, bIndex, strIdent, iIndex);

		if (!ret.empty())
			return ret;

		// Get info on account.
		ret	= Json::Value(Json::objectValue);

		ret["account"]	= naAccount.humanAccountID();
		if (bIndex)
			ret["index"]	= iIndex;

		AccountState::pointer	as		= mNetOps->getAccountState(uLedger, naAccount);
		if (as)
		{
			Json::Value	jsonLines = Json::Value(Json::objectValue);

			ret["account"]	= naAccount.humanAccountID();

			// We access a committed ledger and need not worry about changes.
			uint256	uDirLineNodeFirst;
			uint256	uDirLineNodeLast;

			if (mNetOps->getDirLineInfo(uLedger, naAccount, uDirLineNodeFirst, uDirLineNodeLast))
			{

				for (; uDirLineNodeFirst <= uDirLineNodeLast; uDirLineNodeFirst++)
				{
					STVector256	svRippleNodes	= mNetOps->getDirNode(uLedger, uDirLineNodeFirst);

					BOOST_FOREACH(uint256& uNode, svRippleNodes.peekValue())
					{
						NewcoinAddress	naAccountPeer;
						STAmount		saBalance;
						STAmount		saLimit;
						STAmount		saLimitPeer;

						RippleState::pointer	rsLine	= mNetOps->getRippleState(uLedger, uNode);

						if (rsLine)
						{
							rsLine->setViewAccount(naAccount);

							naAccountPeer		= rsLine->getAccountIDPeer();
							saBalance			= rsLine->getBalance();
							saLimit				= rsLine->getLimit();
							saLimitPeer			= rsLine->getLimitPeer();

							Json::Value				jPeer	= Json::Value(Json::objectValue);

							jPeer["node"]		= uNode.ToString();

							jPeer["balance"]	= saBalance.getText();
							jPeer["currency"]	= saBalance.getCurrencyHuman();
							jPeer["limit"]		= saLimit.getJson(0);
							jPeer["limit_peer"]	= saLimitPeer.getJson(0);

							jsonLines[naAccountPeer.humanAccountID()]	= jPeer;
						}
						else
						{
							std::cerr << "doAccountLines: Bad index: " << uNode.ToString() << std::endl;
						}
					}
				}

			}
			ret["lines"]	= jsonLines;
		}
		else
		{
			ret["status"]	= "NotFound";
		}

		return ret;
	}
}

Json::Value RPCServer::doConnect(Json::Value& params)
{
	// connect <ip> [port]
	std::string strIp;
	int			iPort	= -1;

	if (!params.isArray() || !params.size() || params.size() > 2)
		return JSONRPCError(500, "Invalid parameters");

	// XXX Might allow domain for manual connections.
	if (!extractString(strIp, params, 0))
		return JSONRPCError(500, "Host IP required");

	if (params.size() == 2)
	{
		std::string strPort;

		// YYY Should make an extract int.
		if (!extractString(strPort, params, 1))
			return JSONRPCError(500, "Bad port");

		iPort	= boost::lexical_cast<int>(strPort);
	}

	if (!theApp->getConnectionPool().connectTo(strIp, iPort))
		return "connected";

	return "connecting";
}

Json::Value RPCServer::doPeers(Json::Value& params)
{
	// peers
	return theApp->getConnectionPool().getPeersJson();
}

// credit_set <seed> <paying_account> <destination_account> <limit_amount> [<currency>] [<accept_rate>]
Json::Value RPCServer::doCreditSet(Json::Value& params)
{
	NewcoinAddress	naSeed;
	NewcoinAddress	naSrcAccountID;
	NewcoinAddress	naDstAccountID;
	STAmount		saLimitAmount;
	uint32			uAcceptRate	= params.size() >= 6 ? boost::lexical_cast<uint32>(params[5u].asString()) : 0;

	if (params.size() < 4 || params.size() > 6)
	{
		return JSONRPCError(500, "invalid parameters");
	}
	else if (!naSeed.setFamilySeedGeneric(params[0u].asString()))
	{
		return JSONRPCError(500, "disallowed seed");
	}
	else if (!naSrcAccountID.setAccountID(params[1u].asString()))
	{
		return JSONRPCError(500, "source account id needed");
	}
	else if (!naDstAccountID.setAccountID(params[2u].asString()))
	{
		return JSONRPCError(500, "destination account id needed");
	}
	else if (!saLimitAmount.setValue(params[3u].asString(), params.size() >= 5 ? params[4u].asString() : ""))
	{
		return JSONRPCError(500, "bad src amount/currency");
	}
	else if (!mNetOps->available())
	{
		return JSONRPCError(503, "network not available");
	}
	else
	{
		NewcoinAddress			naMasterGenerator;
		NewcoinAddress			naAccountPublic;
		NewcoinAddress			naAccountPrivate;
	    AccountState::pointer	asSrc;
		uint256					uLedger		= mNetOps->getClosedLedger();
		Json::Value				obj			= authorize(uLedger, naSeed, naSrcAccountID, naAccountPublic, naAccountPrivate, asSrc, naMasterGenerator);

		if (!obj.empty())
			return obj;

		STAmount		saSrcBalance	= asSrc->getBalance();

		if (saSrcBalance < theConfig.FEE_DEFAULT)
		{
			return JSONRPCError(500, "insufficent funds");
		}
		else
		{
			Transaction::pointer	trans	= Transaction::sharedCreditSet(
				naAccountPublic, naAccountPrivate,
				naSrcAccountID,
				asSrc->getSeq(),
				theConfig.FEE_DEFAULT,
				0,											// YYY No source tag
				naDstAccountID,
				saLimitAmount,
				uAcceptRate);

			(void) mNetOps->processTransaction(trans);

			obj["transaction"]		= trans->getSTransaction()->getJson(0);
			obj["status"]			= trans->getStatus();
			obj["seed"]				= naSeed.humanFamilySeed();
			obj["srcAccountID"]		= naSrcAccountID.humanAccountID();
			obj["dstAccountID"]		= naDstAccountID.humanAccountID();
			obj["limitAmount"]		= saLimitAmount.getText();
			obj["acceptRate"]		= uAcceptRate;

			return obj;
		}
	}
}

// send regular_seed paying_account account_id amount [currency] [send_max] [send_currency]
Json::Value RPCServer::doSend(Json::Value& params)
{
	NewcoinAddress	naSeed;
	NewcoinAddress	naSrcAccountID;
	NewcoinAddress	naDstAccountID;
	STAmount		saSrcAmount;
	STAmount		saDstAmount;
	std::string		sSrcCurrency;
	std::string		sDstCurrency;

	if (params.size() >= 5)
		sDstCurrency	= params[4u].asString();

	if (params.size() >= 7)
		sSrcCurrency	= params[6u].asString();

	if (!params.isArray() || params.size() < 3 || params.size() > 7)
	{
		return JSONRPCError(500, "Invalid parameters");
	}
	else if (!naSeed.setFamilySeedGeneric(params[0u].asString()))
	{
		return JSONRPCError(500, "disallowed seed");
	}
	else if (!naSrcAccountID.setAccountID(params[1u].asString()))
	{
		return JSONRPCError(500, "source account id needed");
	}
	else if (!naDstAccountID.setAccountID(params[2u].asString()))
	{
		return JSONRPCError(500, "create account id needed");
	}
	else if (!saDstAmount.setValue(params[3u].asString(), sDstCurrency))
	{
		return JSONRPCError(500, "bad dst amount/currency");
	}
	else if (params.size() >= 6 && !saSrcAmount.setValue(params[5u].asString(), sSrcCurrency))
	{
		return JSONRPCError(500, "bad src amount/currency");
	}
	else if (!mNetOps->available())
	{
		return JSONRPCError(503, "network not available");
	}
	else
	{
		NewcoinAddress			naMasterGenerator;
		NewcoinAddress			naAccountPublic;
		NewcoinAddress			naAccountPrivate;
	    AccountState::pointer	asSrc;
		uint256					uLedger		= mNetOps->getClosedLedger();
		Json::Value				obj			= authorize(uLedger, naSeed, naSrcAccountID, naAccountPublic, naAccountPrivate, asSrc, naMasterGenerator);

		if (!obj.empty())
		{
			return obj;
		}

		if (params.size() < 6)
			saSrcAmount	= saDstAmount;

		// XXX Confirm saSrcAmount >= saDstAmount.

		STAmount				saSrcBalance	= asSrc->getBalance();

		if (saSrcBalance < theConfig.FEE_DEFAULT)
		{
			return JSONRPCError(500, "insufficent funds");
		}
		else
		{
			STPathSet				spPaths;
			Transaction::pointer	trans	= Transaction::sharedPayment(
				naAccountPublic, naAccountPrivate,
				naSrcAccountID,
				asSrc->getSeq(),
				theConfig.FEE_DEFAULT,
				0,											// YYY No source tag
				naDstAccountID,
				saDstAmount,
				saSrcAmount,
				spPaths);

			(void) mNetOps->processTransaction(trans);

			obj["transaction"]		= trans->getSTransaction()->getJson(0);
			obj["status"]			= trans->getStatus();
			obj["seed"]				= naSeed.humanFamilySeed();
			obj["srcAccountID"]		= naSrcAccountID.humanAccountID();
			obj["dstAccountID"]		= naDstAccountID.humanAccountID();
			obj["srcAmount"]		= saSrcAmount.getText();
			obj["srcISO"]			= saSrcAmount.getCurrencyHuman();
			obj["dstAmount"]		= saDstAmount.getText();
			obj["dstISO"]			= saDstAmount.getCurrencyHuman();

			return obj;
		}
	}
}

// transit_set <seed> <paying_account> <transit_rate> <starts> <expires>
Json::Value RPCServer::doTransitSet(Json::Value& params)
{
	NewcoinAddress	naSeed;
	NewcoinAddress	naSrcAccountID;
	std::string		sTransitRate;
	std::string		sTransitStart;
	std::string		sTransitExpire;
	uint32			uTransitRate;
	uint32			uTransitStart;
	uint32			uTransitExpire;

	if (params.size() >= 6)
		sTransitRate		= params[6u].asString();

	if (params.size() >= 7)
		sTransitStart	= params[7u].asString();

	if (params.size() >= 8)
		sTransitExpire	= params[8u].asString();

	if (params.size() != 5)
	{
		return JSONRPCError(500, "invalid parameters");
	}
	else if (!naSeed.setFamilySeedGeneric(params[0u].asString()))
	{
		return JSONRPCError(500, "disallowed seed");
	}
	else if (!naSrcAccountID.setAccountID(params[1u].asString()))
	{
		return JSONRPCError(500, "source account id needed");
	}
	else if (!mNetOps->available())
	{
		return JSONRPCError(503, "network not available");
	}
	else
	{
		NewcoinAddress			naMasterGenerator;
		NewcoinAddress			naAccountPublic;
		NewcoinAddress			naAccountPrivate;
	    AccountState::pointer	asSrc;
		uint256					uLedger		= mNetOps->getClosedLedger();
		Json::Value				obj			= authorize(uLedger, naSeed, naSrcAccountID, naAccountPublic, naAccountPrivate, asSrc, naMasterGenerator);

		if (!obj.empty())
		{
			return obj;
		}

		STAmount		saSrcBalance	= asSrc->getBalance();

		uTransitRate		= 0;
		uTransitStart		= 0;
		uTransitExpire		= 0;

		if (saSrcBalance < theConfig.FEE_DEFAULT)
		{
			return JSONRPCError(500, "insufficent funds");
		}
		else
		{
			Transaction::pointer	trans	= Transaction::sharedTransitSet(
				naAccountPublic, naAccountPrivate,
				naSrcAccountID,
				asSrc->getSeq(),
				theConfig.FEE_DEFAULT,
				0,											// YYY No source tag
				uTransitRate,
				uTransitStart,
				uTransitExpire);

			(void) mNetOps->processTransaction(trans);

			obj["transaction"]		= trans->getSTransaction()->getJson(0);
			obj["status"]			= trans->getStatus();
			obj["seed"]				= naSeed.humanFamilySeed();
			obj["srcAccountID"]		= naSrcAccountID.humanAccountID();
			obj["transitRate"]		= uTransitRate;
			obj["transitStart"]		= uTransitStart;
			obj["transitExpire"]	= uTransitExpire;

			return obj;
		}
	}
	return "Not implemented.";
}

Json::Value RPCServer::doTx(Json::Value& params)
{
	// tx <txID>
	// tx <account>

	std::string param1, param2;
	if (!extractString(param1, params, 0))
	{
		return "bad params";
	}

	if (Transaction::isHexTxID(param1))
	{ // transaction by ID
		Json::Value ret;
		uint256 txid(param1);

		Transaction::pointer txn=theApp->getMasterTransaction().fetch(txid, true);
		if (!txn) return JSONRPCError(500, "Transaction not found");
		return txn->getJson(true);
	}

	return "not implemented";
}

Json::Value RPCServer::doLedger(Json::Value& params)
{
	// ledger
	// ledger <seq>
	// ledger <account>

	int paramCount = getParamCount(params);

	if (paramCount == 0)
	{
		Json::Value ret(Json::objectValue), current(Json::objectValue), closed(Json::objectValue);
		theApp->getMasterLedger().getCurrentLedger()->addJson(current);
		theApp->getMasterLedger().getClosedLedger()->addJson(closed);
		ret["open"] = current;
		ret["closed"] = closed;
		return ret;
	}

	return "not implemented";
}

// unl_add <domain><node_public> [<comment>]
Json::Value RPCServer::doUnlAdd(Json::Value& params)
{
	if (params.size() == 1 || params.size() == 2)
	{
		std::string	strNode		= params[0u].asString();
		std::string strComment	= (params.size() == 2) ? "" : params[1u].asString();

		NewcoinAddress	nodePublic;

		if (nodePublic.setNodePublic(strNode))
		{
			theApp->getUNL().nodeAddPublic(nodePublic, strComment);

			return "adding node by public key";
		}
		else
		{
			theApp->getUNL().nodeAddDomain(strNode, UniqueNodeList::vsManual, strComment);

			return "adding node by domain";
		}
	}
	return "invalid params";
}

// validation_create
// validation_create <pass_phrase>
// validation_create <seed>
// validation_create <seed_key>
//
// NOTE: It is poor security to specify secret information on the command line.  This information might be saved in the command
// shell history file (e.g. .bash_history) and it may be leaked via the process status command (i.e. ps).
Json::Value RPCServer::doValidatorCreate(Json::Value& params) {
	NewcoinAddress	familySeed;
	NewcoinAddress	familyGenerator;
	NewcoinAddress	nodePublicKey;
	NewcoinAddress	nodePrivateKey;

	if (params.size() > 1)
	{
		return "invalid params";
	}
	else if (params.empty())
	{
		std::cerr << "Creating random validation seed." << std::endl;

		familySeed.setFamilySeedRandom();					// Get a random seed.
	}
	else if (!familySeed.setFamilySeedGeneric(params[0u].asString()))
	{
		return "disallowed seed";
	}

	// Derive generator from seed.
	familyGenerator.setFamilyGenerator(familySeed);

	// The node public and private is 0th of the sequence.
	nodePublicKey.setNodePublic(CKey(familyGenerator, 0).GetPubKey());
	nodePrivateKey.setNodePrivate(CKey(familyGenerator, familySeed.getFamilyPrivateKey(), 0).GetSecret());

	// Paranoia
	assert(1 == familySeed.setFamilySeed1751(familySeed.humanFamilySeed1751()));

	Json::Value obj(Json::objectValue);

	obj["validation_public_key"]	= nodePublicKey.humanNodePublic();
	obj["validation_seed"]			= familySeed.humanFamilySeed();
	obj["validation_key"]			= familySeed.humanFamilySeed1751();

	return obj;
}

Json::Value RPCServer::accounts(const uint256& uLedger, const NewcoinAddress& naMasterGenerator)
{
	Json::Value jsonAccounts(Json::objectValue);

	// YYY Don't want to leak to thin server that these accounts are related.
	// YYY Would be best to alternate requests to servers and to cache results.
	uint	uIndex	= 0;

	do {
		NewcoinAddress		naAccount;

		naAccount.setAccountPublic(naMasterGenerator, uIndex++);

		AccountState::pointer as	= mNetOps->getAccountState(uLedger, naAccount);
		if (as)
		{
			as->addJson(jsonAccounts);
		}
		else
		{
			uIndex	= 0;
		}
	} while (uIndex);

	return jsonAccounts;
}

// wallet_accounts <seed>
Json::Value RPCServer::doWalletAccounts(Json::Value& params)
{
	NewcoinAddress	naSeed;
	uint256			uLedger;

	if (params.size() != 1)
	{
		return "invalid params";
	}
	else if (!naSeed.setFamilySeedGeneric(params[0u].asString()))
	{
		return "seed expected";
	}
	else if (!mNetOps->available())
	{
		return JSONRPCError(503, "network not available");
	}
	else if ((uLedger = mNetOps->getClosedLedger()).isZero())
	{
		return JSONRPCError(503, "no closed ledger");
	}

	NewcoinAddress	naMasterGenerator;

	// Try the seed as a master seed.
	naMasterGenerator.setFamilyGenerator(naSeed);

	Json::Value jsonAccounts	= accounts(uLedger, naMasterGenerator);

	if (jsonAccounts.empty())
	{
		// No account via seed as master, try seed a regular.
		Json::Value	ret	= getGenerator(uLedger, naSeed, naMasterGenerator);

		if (!ret.empty())
			return ret;

		ret["accounts"]	= accounts(uLedger, naMasterGenerator);

		return ret;
	}
	else
	{
		// Had accounts via seed as master, return them.
		Json::Value ret(Json::objectValue);

		ret["accounts"]	= jsonAccounts;

		return ret;
	}
}

// wallet_add <regular_seed> <paying_account> <master_seed> [<initial_funds>] [<account_annotation>]
Json::Value RPCServer::doWalletAdd(Json::Value& params)
{
	NewcoinAddress	naMasterSeed;
	NewcoinAddress	naRegularSeed;
	NewcoinAddress	naSrcAccountID;
	STAmount		saAmount;
	std::string		sDstCurrency;

	if (params.size() < 3 || params.size() > 5)
	{
		return "invalid params";
	}
	else if (!naRegularSeed.setFamilySeedGeneric(params[0u].asString()))
	{
		return "regular seed expected";
	}
	else if (!naSrcAccountID.setAccountID(params[1u].asString()))
	{
		return JSONRPCError(500, "source account id needed");
	}
	else if (!naMasterSeed.setFamilySeedGeneric(params[2u].asString()))
	{
		return "master seed expected";
	}
	else if (params.size() >= 4 && !saAmount.setValue(params[3u].asString(), sDstCurrency))
	{
		return JSONRPCError(500, "bad dst amount/currency");
	}
	else if (!mNetOps->available())
	{
		return JSONRPCError(503, "network not available");
	}
	else
	{
		NewcoinAddress			naMasterGenerator;
		NewcoinAddress			naRegularGenerator;

		naMasterGenerator.setFamilyGenerator(naMasterSeed);
		naRegularGenerator.setFamilyGenerator(naRegularSeed);

		NewcoinAddress			naAccountPublic;
		NewcoinAddress			naAccountPrivate;
	    AccountState::pointer	asSrc;
		uint256					uLedger		= mNetOps->getClosedLedger();
		Json::Value				obj			= authorize(uLedger, naRegularSeed, naSrcAccountID, naAccountPublic, naAccountPrivate, asSrc, naMasterGenerator);

		if (!obj.empty())
		{
			return obj;
		}

		// XXX Confirm total funds.

		STAmount				saSrcBalance	= asSrc->getBalance();

		if (saSrcBalance < theConfig.FEE_CREATE)
		{
			return JSONRPCError(500, "insufficent funds");
		}
		else
		{
			NewcoinAddress				naNewAccountPublic;
			NewcoinAddress				naNewAccountPrivate;
			NewcoinAddress				naAuthKeyID;
			uint160						uAuthKeyID;
			AccountState::pointer		asNew;
			std::vector<unsigned char>	vucSignature;
			bool						bAgain	= true;
			int							iIndex	= -1;

			// Find an unmade account.
			do {
				++iIndex;
				naNewAccountPublic.setAccountPublic(naMasterGenerator, iIndex);

				asNew	= mNetOps->getAccountState(uLedger, naNewAccountPublic);
				if (!asNew)
					bAgain	= false;
			} while (bAgain);

			// XXX Have a maximum number of accounts per wallet?

			// Determine corrisponding master private key.
			naNewAccountPrivate.setAccountPrivate(naMasterGenerator, naMasterSeed, iIndex);

			// Determine new accounts authorized regular key.
			naAuthKeyID.setAccountPublic(naRegularGenerator, iIndex);

			uAuthKeyID	= naAuthKeyID.getAccountID();

			// Sign anything (naAuthKeyID) to prove we know new master private key.
			naNewAccountPrivate.accountPrivateSign(Serializer::getSHA512Half(uAuthKeyID.begin(), uAuthKeyID.size()), vucSignature);

			Transaction::pointer	trans	= Transaction::sharedWalletAdd(
				naAccountPublic, naAccountPrivate,
				naSrcAccountID,
				asSrc->getSeq(),
				theConfig.FEE_CREATE,
				0,											// YYY No source tag
				saAmount,
				naAuthKeyID,
				naNewAccountPublic,
				vucSignature);

			(void) mNetOps->processTransaction(trans);

			obj["transaction"]		= trans->getSTransaction()->getJson(0);
			obj["status"]			= trans->getStatus();
			obj["srcAccountID"]		= naSrcAccountID.humanAccountID();
			obj["newAccountID"]		= naNewAccountPublic.humanAccountID();
			obj["amount"]			= saAmount.getText();

			return obj;
		}
	}
}

// wallet_claim <master_seed> <regular_seed> [<source_tag>] [<account_annotation>]
//
// To provide an example to client writers, we do everything we expect a client to do here.
Json::Value RPCServer::doWalletClaim(Json::Value& params)
{
	NewcoinAddress	naMasterSeed;
	NewcoinAddress	naRegularSeed;

	if (params.size() < 2 || params.size() > 4)
	{
		return "invalid params";
	}
	else if (!naMasterSeed.setFamilySeedGeneric(params[0u].asString()))
	{
		// Should also not allow account id's as seeds.
		return "master seed expected";
	}
	else if (!naRegularSeed.setFamilySeedGeneric(params[1u].asString()))
	{
		// Should also not allow account id's as seeds.
		return "regular seed expected";
	}
	else
	{
		// Building:
		//   peer_wallet_claim <account_id> <authorized_key> <encrypted_master_public_generator> <generator_pubkey> <generator_signature>
		//		<source_tag> [<annotation>]
		//
		//
		// Which has no confidential information.

		// XXX Need better parsing.
		uint32		uSourceTag		= (params.size() == 2) ? 0 : boost::lexical_cast<uint32>(params[2u].asString());
		// XXX Annotation is ignored.
		std::string strAnnotation	= (params.size() == 3) ? "" : params[3u].asString();

		NewcoinAddress	naMasterGenerator;
		NewcoinAddress	naRegularGenerator;
		NewcoinAddress	naRegular0Public;
		NewcoinAddress	naRegular0Private;

		NewcoinAddress	naAccountPublic;
		NewcoinAddress	naAccountPrivate;

		naMasterGenerator.setFamilyGenerator(naMasterSeed);
		naAccountPublic.setAccountPublic(naMasterGenerator, 0);
		naAccountPrivate.setAccountPrivate(naMasterGenerator, naMasterSeed, 0);

		naRegularGenerator.setFamilyGenerator(naRegularSeed);

		naRegular0Public.setAccountPublic(naRegularGenerator, 0);
		naRegular0Private.setAccountPrivate(naRegularGenerator, naRegularSeed, 0);

		// Hash of regular account #0 public key.
		uint160						uGeneratorID		= naRegular0Public.getAccountID();
		std::vector<unsigned char>	vucGeneratorCipher	= naRegular0Private.accountPrivateEncrypt(naRegular0Public, naMasterGenerator.getFamilyGenerator());
		std::vector<unsigned char>	vucGeneratorSig;

		// XXX Check result.
		// Prove that we have the corrisponding private key to the generator id.  So, we can get the generator id.
		naRegular0Private.accountPrivateSign(Serializer::getSHA512Half(vucGeneratorCipher), vucGeneratorSig);

		Transaction::pointer	trns	= Transaction::sharedClaim(
			naAccountPublic, naAccountPrivate,
			naAccountPublic,
			uSourceTag,
			vucGeneratorCipher,
			naRegular0Public.getAccountPublic(),
			vucGeneratorSig);

		(void) mNetOps->processTransaction(trns);

		Json::Value obj(Json::objectValue);

		// We "echo" the seeds so they can be checked.
		obj["master_seed"]		= naMasterSeed.humanFamilySeed();
		obj["master_key"]		= naMasterSeed.humanFamilySeed1751();
		obj["regular_seed"]		= naRegularSeed.humanFamilySeed();
		obj["regular_key"]		= naRegularSeed.humanFamilySeed1751();

		obj["account_id"]		= naAccountPublic.humanAccountID();
		obj["generator_id"]		= strHex(uGeneratorID);
		obj["generator"]		= strHex(vucGeneratorCipher);
		obj["annotation"]		= strAnnotation;

		obj["transaction"]		= trns->getSTransaction()->getJson(0);
		obj["status"]			= trns->getStatus();

		return obj;
	}
}

// wallet_create regular_seed paying_account account_id [initial_funds]
// We don't allow creating an account_id by default here because we want to make sure the person has a chance to write down the
// master seed of the account to be created.
// YYY Need annotation and source tag
Json::Value RPCServer::doWalletCreate(Json::Value& params)
{
	NewcoinAddress	naSrcAccountID;
	NewcoinAddress	naDstAccountID;
	NewcoinAddress	naSeed;
	uint256			uLedger;

	if (params.size() < 3 || params.size() > 4)
	{
		return "invalid params";
	}
	else if (!naSeed.setFamilySeedGeneric(params[0u].asString()))
	{
		return "disallowed seed";
	}
	else if (!naSrcAccountID.setAccountID(params[1u].asString()))
	{
		return "source account id needed";
	}
	else if (!naDstAccountID.setAccountID(params[2u].asString()))
	{
		return "create account id needed";
	}
	else if (!mNetOps->available())
	{
		// We require access to the paying account's sequence number and key information.
		return JSONRPCError(503, "network not available");
	}
	else if ((uLedger = mNetOps->getClosedLedger()).isZero())
	{
		return JSONRPCError(503, "no closed ledger");
	}
	else if (mNetOps->getAccountState(uLedger, naDstAccountID))
	{
		return "account already exists";
	}
	else
	{
		// Trying to build:
		//   peer_wallet_create <paying_account> <paying_signature> <account_id> [<initial_funds>] [<annotation>]

		NewcoinAddress			naMasterGenerator;
		NewcoinAddress			naAccountPublic;
		NewcoinAddress			naAccountPrivate;
	    AccountState::pointer	asSrc;
		uint256					uLedger			= mNetOps->getClosedLedger();
		Json::Value				obj				= authorize(uLedger, naSeed, naSrcAccountID, naAccountPublic, naAccountPrivate, asSrc, naMasterGenerator);

		STAmount				saSrcBalance	= asSrc->getBalance();
		STAmount				saInitialFunds	= (params.size() < 4) ? 0 : boost::lexical_cast<uint64>(params[3u].asString());

		if (!obj.empty())
		{
			return obj;
		}
		else if (saSrcBalance < (saInitialFunds + theConfig.FEE_CREATE))
		{
			return JSONRPCError(500, "insufficent funds");
		}
		else
		{
			Transaction::pointer	trans	= Transaction::sharedCreate(
				naAccountPublic, naAccountPrivate,
				naSrcAccountID,
				asSrc->getSeq(),
				theConfig.FEE_CREATE,
				0,											// YYY No source tag
				naDstAccountID,
				saInitialFunds);							// Initial funds in XNC.

			(void) mNetOps->processTransaction(trans);

			obj["transaction"]		= trans->getSTransaction()->getJson(0);
			obj["status"]			= trans->getStatus();

			return obj;
		}
	}
}

// wallet_propose
Json::Value RPCServer::doWalletPropose(Json::Value& params)
{
	if (params.size())
	{
		return "invalid params";
	}
	else
	{
		NewcoinAddress	naSeed;
		NewcoinAddress	naGenerator;
		NewcoinAddress	naAccount;

		naSeed.setFamilySeedRandom();
		naGenerator.setFamilyGenerator(naSeed);
		naAccount.setAccountPublic(naGenerator, 0);

		Json::Value obj(Json::objectValue);

		obj["master_seed"]		= naSeed.humanFamilySeed();
		obj["master_key"]		= naSeed.humanFamilySeed1751();
		obj["account_id"]		= naAccount.humanAccountID();

		return obj;
	}
}

// wallet_seed [<seed>|<passphrase>|<passkey>]
Json::Value RPCServer::doWalletSeed(Json::Value& params)
{
	NewcoinAddress	naSeed;

	if (params.size() > 1)
	{
		return "invalid params";
	}
	else if (params.size()
		&& !naSeed.setFamilySeedGeneric(params[0u].asString()))
	{
		return "disallowed seed";
	}
	else
	{
		NewcoinAddress	naGenerator;
		NewcoinAddress	naAccount;

		if (!params.size())
		{
			naSeed.setFamilySeedRandom();
		}
		naGenerator.setFamilyGenerator(naSeed);
		naAccount.setAccountPublic(naGenerator, 0);

		Json::Value obj(Json::objectValue);

		obj["seed"]		= naSeed.humanFamilySeed();
		obj["key"]		= naSeed.humanFamilySeed1751();

		return obj;
	}
}

void RPCServer::validatorsResponse(const boost::system::error_code& err, std::string strResponse)
{
	std::cerr << "Fetch '" VALIDATORS_FILE_NAME "' complete." << std::endl;

	if (!err)
	{
		theApp->getUNL().nodeDefault(strResponse);
	}
	else
	{
		std::cerr << "Error: " << err.message() << std::endl;
	}
}

// Populate the UNL from a validators.txt file.
Json::Value RPCServer::doUnlDefault(Json::Value& params) {
	if (!params.size() || (1==params.size() && !params[0u].compare("network")))
	{
		bool			bNetwork	= 1 == params.size();
		std::string		strValidators;

		if (!bNetwork)
		{
			std::ifstream	ifsDefault(VALIDATORS_FILE_NAME, std::ios::in);

			if (!ifsDefault)
			{
				std::cerr << "Failed to open '" VALIDATORS_FILE_NAME "'." << std::endl;

				bNetwork	= true;
			}
			else
			{
				strValidators.assign((std::istreambuf_iterator<char>(ifsDefault)),
					std::istreambuf_iterator<char>());

				if (ifsDefault.bad())
				{
					std::cerr << "Failed to read '" VALIDATORS_FILE_NAME "'." << std::endl;

					bNetwork	= true;
				}
			}
		}

		if (bNetwork)
		{
			HttpsClient::httpsGet(
				theApp->getIOService(),
				VALIDATORS_SITE,
				443,
				VALIDATORS_FILE_PATH,
				VALIDATORS_FILE_BYTES_MAX,
				boost::posix_time::seconds(VALIDATORS_FETCH_SECONDS),
				boost::bind(&RPCServer::validatorsResponse, this, _1, _2));

			return "fetching " VALIDATORS_FILE_NAME;
		}
		else
		{
			theApp->getUNL().nodeDefault(strValidators);

			return "processing " VALIDATORS_FILE_NAME;
		}
	}
	else return "invalid params";
}

// unl_delete <public_key>
Json::Value RPCServer::doUnlDelete(Json::Value& params) {
	if (1 == params.size())
	{
		std::string	strNodePublic = params[0u].asString();

		NewcoinAddress	naNodePublic;

		if (naNodePublic.setNodePublic(strNodePublic))
		{
			theApp->getUNL().nodeRemove(naNodePublic);

			return "removing node";
		}
		else
		{
			return "invalid public key";
		}
	}
	else return "invalid params";
}

Json::Value RPCServer::doUnlList(Json::Value& params) {
	return theApp->getUNL().getUnlJson();
}

// unl_reset
Json::Value RPCServer::doUnlReset(Json::Value& params) {
	if (!params.size())
	{
		theApp->getUNL().nodeReset();

		return "removing nodes";
	}
	else return "invalid params";
}

// unl_score
Json::Value RPCServer::doUnlScore(Json::Value& params) {
	if (!params.size())
	{
		theApp->getUNL().nodeScore();

		return "scoring requested";
	}
	else return "invalid params";
}

Json::Value RPCServer::doStop(Json::Value& params) {
	if (!params.size())
	{
		theApp->stop();

		return SYSTEM_NAME " server stopping";
	}
	else return "invalid params";
}

Json::Value RPCServer::doCommand(const std::string& command, Json::Value& params)
{
	std::cerr << "RPC:" << command << std::endl;

	if (command == "account_email_set")		return doAccountEmailSet(params);
	if (command == "account_info")			return doAccountInfo(params);
	if (command == "account_lines")			return doAccountLines(params);
	if (command == "connect")				return doConnect(params);
	if (command == "credit_set")			return doCreditSet(params);
	if (command == "peers")					return doPeers(params);
	if (command == "send")					return doSend(params);
	if (command == "stop")					return doStop(params);
	if (command == "transit_set")			return doTransitSet(params);

	if (command == "unl_add")				return doUnlAdd(params);
	if (command == "unl_default")			return doUnlDefault(params);
	if (command == "unl_delete")			return doUnlDelete(params);
	if (command == "unl_list")				return doUnlList(params);
	if (command == "unl_reset")				return doUnlReset(params);
	if (command == "unl_score")				return doUnlScore(params);

	if (command == "validation_create")		return doValidatorCreate(params);

	if (command == "wallet_accounts")		return doWalletAccounts(params);
	if (command == "wallet_add")			return doWalletAdd(params);
	if (command == "wallet_claim")			return doWalletClaim(params);
	if (command == "wallet_create")			return doWalletCreate(params);
	if (command == "wallet_propose")		return doWalletPropose(params);
	if (command == "wallet_seed")			return doWalletSeed(params);

	//
	// Obsolete or need rewrite:
	//

	if (command=="tx") return doTx(params);
	if (command=="ledger") return doLedger(params);

	return "unknown command";
}

void RPCServer::sendReply()
{
	boost::asio::async_write(mSocket, boost::asio::buffer(mReplyStr),
			boost::bind(&RPCServer::handle_write, shared_from_this(),
			boost::asio::placeholders::error));
}

void RPCServer::handle_write(const boost::system::error_code& /*error*/)
{
}

// vim:ts=4
