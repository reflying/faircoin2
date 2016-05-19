// Copyright (c) 2016 The FairCoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "base58.h"
#include "clientversion.h"
#include "init.h"
#include "main.h"
#include "net.h"
#include "netbase.h"
#include "rpcserver.h"
#include "timedata.h"
#include "util.h"
#include "utilstrencodings.h"
#include "tinyformat.h"
#include "poc.h"
#include "consensus/validation.h"
#include "consensus/merkle.h"
#ifdef ENABLE_WALLET
#include "wallet/wallet.h"
#include "wallet/walletdb.h"
#endif

#include <stdint.h>
#include <boost/assign/list_of.hpp>
#include <boost/algorithm/string.hpp>
#include <univalue.h>
#include <iostream>
#include <string>

using namespace std;


static bool AddAdminSignatures(CChainDataMsg &msg, const UniValue& sigs)
{
    if (sigs.size() < (size_t)dynParams.nMinCvnSigners)
        throw runtime_error(
            strprintf("not enough signatures supplied "
                      "(got %u signatures, but need at least %u to sign)", sigs.size(), (size_t)dynParams.nMinCvnSigners));
    if (sigs.size() > (size_t)dynParams.nMaxCvnSigners)
        throw runtime_error(
            strprintf("too many signatures supplied %u (%u max)\nReduce the number", sigs.size(), (size_t)dynParams.nMaxCvnSigners));

    msg.vAdminSignatures.resize(sigs.size());

    for (uint32_t i = 0 ; i < sigs.size() ; i++)
    {
        const string& strSig = sigs[i].get_str();
        vector<string> vTokens;
        boost::split(vTokens, strSig, boost::is_any_of(":"));

        if (vTokens.size() != 2)
            throw runtime_error(strprintf("signature %u is of invalid format", i + 1));

        uint32_t signerId;
        stringstream ss;
        ss << hex << vTokens[0].c_str();
        ss >> signerId;

        msg.vAdminSignatures[i] = CCvnSignature(signerId, ParseHex(vTokens[1]));
    }

    return CheckAdminSignatures(msg.GetHash(), msg.vAdminSignatures);
}

static void AddCvnInfoToMsg(CChainDataMsg &msg, const uint32_t nNodeId, const uint32_t nHeightAdded, const vector<unsigned char> vPubKey)
{
    msg.nPayload |= CChainDataMsg::CVN_PAYLOAD;
    msg.vCvns.resize(mapCVNs.size() + 1);

    uint32_t index = 0;
    BOOST_FOREACH(const CvnMapType::value_type& cvn, mapCVNs)
    {
        msg.vCvns[index++] = cvn.second;
    };

    CCvnInfo cvn(nNodeId, nHeightAdded, vPubKey);
    msg.vCvns[index] = cvn;
}

static void AddChainAdminToMsg(CChainDataMsg &msg, const uint32_t nAdminId, const vector<unsigned char> vPubKey)
{
    msg.nPayload |= CChainDataMsg::CHAIN_ADMINS_PAYLOAD;
    msg.vChainAdmins.resize(mapChainAdmins.size() + 1);

    uint32_t index = 0;
    BOOST_FOREACH(const ChainAdminMapType::value_type& cvn, mapChainAdmins)
    {
        msg.vChainAdmins[index++] = cvn.second;
    };

    CChainAdmin admin(nAdminId, vPubKey);
    msg.vChainAdmins[index] = admin;
}

static void AddDynParamsToMsg(CChainDataMsg& msg, UniValue jsonParams)
{
    LogPrintf("AddDynParamsToBlock : adding %u parameters\n", jsonParams.getKeys().size());
    msg.nPayload |= CChainDataMsg::CHAIN_PARAMETERS_PAYLOAD;

    CDynamicChainParams& params = msg.dynamicChainParams;

    params.nBlockSpacing            = dynParams.nBlockSpacing;
    params.nBlockSpacingGracePeriod = dynParams.nBlockSpacingGracePeriod;
    params.nDustThreshold           = dynParams.nDustThreshold;
    params.nMaxCvnSigners           = dynParams.nMaxCvnSigners;
    params.nMinCvnSigners           = dynParams.nMinCvnSigners;
    params.nMinSuccessiveSignatures = dynParams.nMinSuccessiveSignatures;

    vector<string> paramsList = jsonParams.getKeys();
    BOOST_FOREACH(const string& key, paramsList) {
        LogPrintf("AddDynParamsToBlock : adding %s: %u\n", key, jsonParams[key].get_int());
        if (key == "nBlockSpacing") {
            params.nBlockSpacing = jsonParams[key].get_int();
        } else if (key == "nBlockSpacingGracePeriod") {
            params.nBlockSpacingGracePeriod = jsonParams[key].get_int();
        } else if (key == "nDustThreshold") {
            params.nDustThreshold = jsonParams[key].get_int();
        } else if (key == "nMaxCvnSigners") {
            params.nMaxCvnSigners = jsonParams[key].get_int();
        } else if (key == "nMinCvnSigners") {
            params.nMinCvnSigners = jsonParams[key].get_int();
        } else if (key == "nMinSuccessiveSignatures") {
            params.nMinSuccessiveSignatures = jsonParams[key].get_int();
        }
    }
}

UniValue addcvn(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() < 4 || params.size() > 5)
        throw runtime_error(
            "addcvn \"type\" \"Id\" \"timestamp\" \"pubkey\" [\"n:sigs\",...] {\"nParam1\":123,\"nParam2\":456}\n"
            "\nAdd a new CVN to the FairCoin network\n"
            "\nArguments:\n"
            "1. \"type\"               (string, required) c=CVNInfo, a=ChainAdmin\n"
            "2. \"Id\"                 (string, required) The ID (in hex) of the new CVN or admin.\n"
            "3. \"pubkey\"             (string, required but can be empty) The public key of the new CVN or Chain Admin (in hex).\n"
            "4. \"[n:sigs]\"           (string, required) The admin signatures prefix by the signer ID (n)\n"
            "5. \"{\"key\":\"val\"}]\" (string, optional) The dynamic chain parameters to set)\n"
            "\nResult:\n"
            "{\n"
                "  \"type\":\"type of added info\",             (string) The type of the added info (c=CVNInfo, a=ChainAdmin)\n"
                "  \"Id\":\"ID in hex\",                    (hex) The ID of the new CVN (or admin) in hexadecimal form\n"
                "  \"prevBlockHash\":\"hash (hex)\",            (string) The timestamp of the block\n"
                "  \"address\":\"faircoin address\",            (string) The FairCoin address of the new CVN.\n"
                "  \"pubKey\":\"public key\",                   (string) The public key of the new CVN (in hex).\n"
                "  \"signatures\":\"number of signatures\"      (string) The number of admin signatures that signed the CvnInfo.\n"
                "  \"chainParams\":\"serialized params\"        (string) The serialized representation of CDynamicChainParams.\n"
             "}\n"
            "\nExamples:\n"
            "\nAdd a new CVN\n"
            + HelpExampleCli("addcvn", "c 0x123488 1461056246 \"04...00\" [\\\"0x87654321:a1b5..9093\\\",\\\"0xdeadcafe:0432..12aa\\\"] \"{\\\"nParapm1\\\":\\\"123\\\",\\\"nParapm2\\\":\\\"456\\\"}")
        );

    LOCK(cs_main);
    UniValue result(UniValue::VOBJ);
    bool fAddCvn = true;
    if (params[0].get_str() == "a")
        fAddCvn = false;

    uint32_t nNodeId;
    stringstream ss;
    ss << hex << params[1].get_str();
    ss >> nNodeId;

    vector<unsigned char> vPubKey = ParseHex(params[2].get_str());
    CPubKey pubKey(vPubKey);

    if (!pubKey.IsFullyValid() && params[4].isNull())
        throw runtime_error(" Invalid public key: " + params[2].get_str());

    const UniValue& sigs = params[3].get_array();

    CChainDataMsg msg;
    msg.hashPrevBlock = chainActive.Tip()->GetBlockHash();

    if (pubKey.IsFullyValid()) {
        if (fAddCvn)
            AddCvnInfoToMsg(msg, nNodeId, chainActive.Tip()->nHeight + 1, vPubKey);
        else
            AddChainAdminToMsg(msg, nNodeId, vPubKey);
    }

    if (params[4].isObject() && !params[4].get_obj().getKeys().empty())
        AddDynParamsToMsg(msg, params[4].get_obj());

    // if no signatures are supplied we print out the CChainDataMsg's hash to sign
    if (!sigs.size())
        return msg.GetHash().ToString();

    if (!AddAdminSignatures(msg, sigs))
        return "error in signatures";

    result.push_back(Pair("nodeId", strprintf("0x%08x", nNodeId)));

    if (msg.HasCvnInfo()) {
        CKeyID keyID = pubKey.GetID();
        CBitcoinAddress address;
        address.Set(keyID);

        LogPrintf("about to add CVN 0x%08x with pubKey %s (%s) to the network\n", nNodeId, HexStr(vPubKey), address.ToString());
        result.push_back(Pair("pubKey", HexStr(vPubKey)));
        result.push_back(Pair("address", address.ToString()));
    }

    if (msg.HasChainParameters()) {
        LogPrintf("about to update dynamic chain parameters on the network\n   %s\n", msg.dynamicChainParams.ToString());
        result.push_back(Pair("dynamicChainParams", msg.dynamicChainParams.ToString()));
    }

    if (msg.HasChainAdmins()) {
        LogPrintf("about to add chain admin 0x%08x with pubKey %s to the network\n", nNodeId, HexStr(vPubKey));
        result.push_back(Pair("pubKey", HexStr(vPubKey)));
    }

    if (IsInitialBlockDownload())
        return "wait for block chain download to finish";

    if (AddChainData(msg)) {
        RelayChainData(msg);
    } else
         LogPrintf("ERROR\n%s\n", msg.ToString());

    return result;
}

UniValue removecvn(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 3)
        throw runtime_error(
            "removecvn \"Id\" \"timestamp\" [\"n:sigs\",...]\n"
            "\nRemove a CVN from the FairCoin network\n"
            "\nArguments:\n"
            "1. \"type\"         (string, required) c=CVNInfo, a=ChainAdmin\n"
            "2. \"Id\"           (string, required) The ID (in hex) of the CVN or admin to remove.\n"
            "3. \"n:sigs\"       (string, required) The admin signatures prefix by the signer ID (n)\n"
            "\nResult:\n"
            "{\n"
                "  \"type\":\"type of info\",                   (string) The type of the info (c=CVNInfo, a=ChainAdmin)\n"
                "  \"Id\":\"node ID (hex)\",                    (string) The ID  of the new CVN in hex separated by a space\n"
             "}\n"
            "\nExamples:\n"
            "\nRemove a CVN\n"
            + HelpExampleCli("removecvn", "c 0x123488 [\"0x87654321:a1b5..9093\",\"0x3453:0432..12aa\"]")
        );

    LOCK(cs_main);

    bool fRemoveCvn = true;
    if (params[0].get_str() == "a")
        fRemoveCvn = false;

    uint32_t nNodeId;
    stringstream ss;
    ss << hex << params[1].get_str();
    ss >> nNodeId;

    const UniValue& sigs = params[2].get_array();

    CChainDataMsg msg;
    msg.nPayload      |= (fRemoveCvn ? CChainDataMsg::CVN_PAYLOAD : CChainDataMsg::CHAIN_ADMINS_PAYLOAD);
    msg.hashPrevBlock  = chainActive.Tip()->GetBlockHash();

    if (msg.HasCvnInfo()) {
        LOCK(cs_mapCVNs);
        msg.vCvns.resize(mapCVNs.size() - 1);

        if (!mapCVNs.count(nNodeId))
            throw runtime_error("CVN ID not found");

        uint32_t index = 0;
        BOOST_FOREACH(const CvnMapType::value_type& cvn, mapCVNs)
        {
            if (cvn.first != nNodeId)
                msg.vCvns[index++] = cvn.second;
        };
    } else {
        LOCK(cs_mapChainAdmins);
        msg.vChainAdmins.resize(mapChainAdmins.size() - 1);

        if (!mapChainAdmins.count(nNodeId))
            throw runtime_error("Admin ID not found");

        uint32_t index = 0;
        BOOST_FOREACH(const ChainAdminMapType::value_type& adm, mapChainAdmins)
        {
            if (adm.first != nNodeId)
                msg.vChainAdmins[index++] = adm.second;
        };
    }

    if (IsInitialBlockDownload())
        return "wait for block chain download to finish";

    // if no signatures are supplied we print out the CChainDataMsg's hash to sign
    if (!sigs.size())
        return msg.GetHash().ToString();

    AddAdminSignatures(msg, sigs);
    LogPrintf("about remove %s 0x%08x from the network\n", fRemoveCvn ? "CVN" : "Admin", nNodeId);;

    if (AddChainData(msg)) {
        RelayChainData(msg);
    }

    UniValue result(UniValue::VOBJ);
    result.push_back(Pair("Id", strprintf("0x%08x", nNodeId)));

    return result;
}

UniValue signchaindata(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 3)
        throw runtime_error(
            "signchaindata \"signchaindata\"\n"
            "\nCreates a signature of chain data\n"
            "\nArguments:\n"
            "1. \"hashChainData\"   (string, required) The hash of the chain data.\n"
            "2. \"adminId\"         (string, required) The admin ID (hex)\n"
            "3. \"privKey\"         (string, required) The private key of the chain admin\n"
            "\nExamples:\n"
            "\nCreate a signature\n"
            + HelpExampleCli("signchaindata", "a1b5..9093")
        );

    LOCK(cs_main);

    uint256 hashChainData = uint256S(params[0].get_str());

    uint32_t nAdminId;
    stringstream ss;
    ss << hex << params[1].get_str();
    ss >> nAdminId;

    vector<unsigned char> vSignature;

    CBitcoinSecret secret;
    if (!secret.SetString(params[2].get_str()))
        return "private key is invalid";

    CKey key = secret.GetKey();
    if (!key.Sign(hashChainData, vSignature))
        return "CvnSignWithKey : could not create block signature";

    CCvnSignature sig(nAdminId, vSignature);

    if (!CvnVerifyAdminSignature(hashChainData, sig)) {
        return "error signing chain data";
    }

    return "\"" + params[1].get_str() + ":" + HexStr(vSignature) + "\"";
}

UniValue getcvninfo(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw runtime_error(
            "getcvninfo\n"
            "\nDisplay the current state of the CVN\n"
            "\nArguments:\n"
            "none\n"
            "\nResult:\n"
            "{\n"
                "  \"nextBlockToCreate\":height     ,           (int) The estimated next block to create\n"
                "  \"reserved\":\"reserved\",                   (string) reserved\n"
             "}\n"
            "\nExamples:\n"
            "\nDisplay CVN state\n"
            + HelpExampleCli("getcvninfo","")
        );

    LOCK(cs_main);

    return "to be implemented";
}
