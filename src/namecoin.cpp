#include <vector>
using namespace std;

#include "script.h"
#include "wallet.h"
#include "base58.h"
extern CWallet* pwalletMain;
extern std::map<uint256, CTransaction> mapTransactions;

#include "namecoin.h"
#include "hooks.h"

#include <boost/xpressive/xpressive_dynamic.hpp>

using namespace json_spirit;

template<typename T> void ConvertTo(Value& value, bool fAllowNull=false);

map<vector<unsigned char>, uint256> mapMyNames;
map<vector<unsigned char>, set<uint256> > mapNamePending;

extern uint256 SignatureHash(CScript scriptCode, const CTransaction& txTo, unsigned int nIn, int nHashType);

// forward decls
extern bool Solver(const CKeyStore& keystore, const CScript& scriptPubKey, uint256 hash, int nHashType, CScript& scriptSigRet, txnouttype& whichTypeRet);
extern bool VerifyScript(const CScript& scriptSig, const CScript& scriptPubKey, const CTransaction& txTo, unsigned int nIn, bool fValidatePayToScriptHash, int nHashType);
extern void rescanfornames();
extern std::string _(const char* psz);
extern bool ThreadSafeAskFee(int64 nFeeRequired, const std::string& strCaption);

const int NAME_COIN_GENESIS_EXTRA = 521;

class CNamecoinHooks : public CHooks
{
public:
    virtual bool IsStandard(const CTransaction &tx);
    virtual bool CheckTransaction(const CTransaction& tx);
    virtual bool ConnectInputs(CTxDB& txdb,
            map<uint256, CTxIndex>& mapTestPool,
            const CTransaction& tx,
            vector<CTransaction>& vTxPrev,
            vector<CTxIndex>& vTxindex,
            const CBlockIndex* pindexBlock,
            const CDiskTxPos &txPos,
            bool fBlock,
            bool fMiner);
    virtual bool DisconnectInputs(CTxDB& txdb,
            const CTransaction& tx,
            CBlockIndex* pindexBlock);
    virtual bool ConnectBlock(CBlock& block, CTxDB& txdb, CBlockIndex* pindex);
    virtual bool DisconnectBlock(CBlock& block, CTxDB& txdb, CBlockIndex* pindex);
    virtual bool ExtractAddress(const CScript& script, string& address);
    virtual void AcceptToMemoryPool(CTxDB& txdb, const CTransaction& tx);

    virtual bool IsMine(const CTransaction& tx);
    virtual bool IsMine(const CTransaction& tx, const CTxOut& txout);

    virtual bool SelectCoinsMinConf(const CWalletTx *pcoin, int nVersion);
    virtual bool listunspent(int nVersion);
};

vector<unsigned char> vchFromValue(const Value& value) {
    string strName = value.get_str();
    unsigned char *strbeg = (unsigned char*)strName.c_str();
    return vector<unsigned char>(strbeg, strbeg + strName.size());
}

vector<unsigned char> vchFromString(const string &str) {
    unsigned char *strbeg = (unsigned char*)str.c_str();
    return vector<unsigned char>(strbeg, strbeg + str.size());
}

string stringFromVch(const vector<unsigned char> &vch) {
    string res;
    vector<unsigned char>::const_iterator vi = vch.begin();
    while (vi != vch.end()) {
        res += (char)(*vi);
        vi++;
    }
    return res;
}

//how much time (in blocks) this name can still be used
bool GetNameTotalLifeTime(CNameDB& dbName, const vector<unsigned char> &vchName, int& nTotalLifeTime)
{
    vector<CNameIndex> vtxPos;
    if (!dbName.ExistsName(vchName))
        return error("GetNameTotalLifeTime() : name - %s - does not exist in name DB", stringFromVch(vchName).c_str());
    if (!dbName.ReadName(vchName, vtxPos))
        return error("GetNameTotalLifeTime() : failed to read from name DB");

    int64 sum = 0;
    BOOST_FOREACH(const CNameIndex& txPos, vtxPos)
    {
        CTransaction tx;
        if (!tx.ReadFromDisk(txPos.txPos))
            return error("GetNameTotalLifeTime() : could not read tx from disk");

        NameTxInfo nti;
        if (!DecodeNameTx(tx, nti, false))      //we don't care about values correctness here
            return error("GetNameTotalLifeTime() : not namecoin tx, this should never happen");

        sum += nti.nRentalDays*144; //days to blocks. 144 = (24*6) - assumping 1 block every 10 minutes
    }
    nTotalLifeTime = sum > 1000000000 ? 1000000000 : sum; //upper limit is 1 billion. this should fit in 2^32
    return true;
}

bool GetNameTotalLifeTime(const vector<unsigned char> &vchName, int& nTotalLifeTime)
{
    CNameDB dbName("r");
    return GetNameTotalLifeTime(dbName, vchName, nTotalLifeTime);
}

//name total lifetime (nTotalLifeTime) and block number at which name was registered (nHeight)
bool GetExpirationData(CNameDB& dbName,const vector<unsigned char> &vchName, int& nTotalLifeTime, int& nHeight)
{
    if (!GetNameTotalLifeTime(dbName, vchName, nTotalLifeTime))
        return false;

    nHeight = GetNameHeight(dbName, vchName);
    if (nHeight <= 0) return false;

    return true;
}

bool GetExpirationData(const vector<unsigned char> &vchName, int& nTotalLifeTime, int& nHeight)
{
    CNameDB dbName("r");
    return GetExpirationData(dbName, vchName, nTotalLifeTime, nHeight);
}

//returns minimum name_new fee rounded down to cents
int64 GetNameNewFee(const CBlockIndex* pindexBlock, const int nRentalDays)
{
    const CBlockIndex* lastPoW = GetLastBlockIndex(pindexBlock, false);

    int64 txMinFee = nRentalDays * lastPoW->nMint / (365 * 100); //1% per 365 days
    txMinFee += lastPoW->nMint / 100; //+1% per operation itself
    txMinFee = sqrt(txMinFee / CENT) * CENT; //Square root is taken of the number of cents. This means that txMinFeee should be at least 1 cent.

    // Round up to CENT
    txMinFee += CENT - 1;
    txMinFee = (txMinFee / CENT) * CENT;

    txMinFee = max(txMinFee, MIN_TX_FEE);

    return txMinFee;
}

int64 GetNameUpdateFee(const CBlockIndex* pindexBlock, const int nRentalDays)
{
    const CBlockIndex* lastPoW = GetLastBlockIndex(pindexBlock, false);
    int64 txMinFee = nRentalDays * lastPoW->nMint / (365 * 100); //1% per 365 days
    txMinFee = sqrt(txMinFee / CENT) * CENT; //Square root is taken of the number of cents. This means that txMinFeee should be at least 1 cent.

    // Round up to CENT
    txMinFee += CENT - 1;
    txMinFee = (txMinFee / CENT) * CENT;

    txMinFee = max(txMinFee, MIN_TX_FEE);

    return txMinFee;
}

int GetTxPosHeight(const CNameIndex& txPos)
{
    return txPos.nHeight;
}

int GetTxPosHeight(const CDiskTxPos& txPos)
{
    // Read block header
    CBlock block;
    if (!block.ReadFromDisk(txPos.nFile, txPos.nBlockPos, false))
        return 0;
    // Find the block in the index
    map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(block.GetHash());
    if (mi == mapBlockIndex.end())
        return 0;
    CBlockIndex* pindex = (*mi).second;
    if (!pindex || !pindex->IsInMainChain())
        return 0;
    return pindex->nHeight;
}

int GetTxPosHeight2(const CDiskTxPos& txPos, int nHeight)
{
    nHeight = GetTxPosHeight(txPos);
    return nHeight;
}

int GetNameHeight(CTxDB& txdb, vector<unsigned char> vchName) {
    CNameDB dbName("cr", txdb);
    vector<CNameIndex> vtxPos;
    if (dbName.ExistsName(vchName))
    {
        if (!dbName.ReadName(vchName, vtxPos))
            return error("GetNameHeight() : failed to read from name DB");
        if (vtxPos.empty())
            return -1;
        CNameIndex& txPos = vtxPos.front();
        return GetTxPosHeight(txPos);
    }
    return -1;
}

int GetNameHeight(CNameDB& dbName, vector<unsigned char> vchName) {
    vector<CNameIndex> vtxPos;
    if (dbName.ExistsName(vchName))
    {
        if (!dbName.ReadName(vchName, vtxPos))
            return error("GetNameHeight() : failed to read from name DB");
        if (vtxPos.empty())
        {
            printf("GetNameHeight() : this name is not in nameindex.dat\n");
            return -1;
        }
        CNameIndex& txPos = vtxPos.front();
        return GetTxPosHeight(txPos);
    }
    return -1;
}

CScript RemoveNameScriptPrefix(const CScript& scriptIn)
{
    NameTxInfo nti;
    CScript::const_iterator pc = scriptIn.begin();

    if (!DecodeNameScript(scriptIn, nti, pc))
        throw runtime_error(nti.err_msg);
    return CScript(pc, scriptIn.end());
}

bool SignNameSignature(const CTransaction& txFrom, CTransaction& txTo, unsigned int nIn, int nHashType=SIGHASH_ALL, CScript scriptPrereq=CScript())
{

    {
        CTxDB txdb("r");
        CTxIndex txInd;
        txdb.ReadTxIndex(txFrom.GetHash(),txInd);
        printf("txFrom depth = %d", txInd.GetDepthInMainChain());
    }


    assert(nIn < txTo.vin.size());
    CTxIn& txin = txTo.vin[nIn];
    assert(txin.prevout.n < txFrom.vout.size());
    const CTxOut& txout = txFrom.vout[txin.prevout.n];

    // Leave out the signature from the hash, since a signature can't sign itself.
    // The checksig op will also drop the signatures from its hash.

    const CScript& scriptPubKey = RemoveNameScriptPrefix(txout.scriptPubKey);
    uint256 hash = SignatureHash(scriptPrereq + txout.scriptPubKey, txTo, nIn, nHashType);

    txnouttype whichType;
    if (!Solver(*pwalletMain, scriptPubKey, hash, nHashType, txin.scriptSig, whichType))
        return false;

    txin.scriptSig = scriptPrereq + txin.scriptSig;

    // Test solution
    if (scriptPrereq.empty())
        if (!VerifyScript(txin.scriptSig, txout.scriptPubKey, txTo, nIn, false, 0))
            return false;

    return true;
}

bool CreateTransactionWithInputTx(const vector<pair<CScript, int64> >& vecSend, CWalletTx& wtxIn, int nTxOut, CWalletTx& wtxNew, CReserveKey& reservekey, int64& nFeeRet)
{
    int64 nValue = 0;
    BOOST_FOREACH(const PAIRTYPE(CScript, int64)& s, vecSend)
    {
        if (nValue < 0)
            return false;
        nValue += s.second;
    }
    if (vecSend.empty() || nValue < 0)
        return false;

    wtxNew.BindWallet(pwalletMain);

    {
        LOCK2(cs_main, pwalletMain->cs_wallet);
        // txdb must be opened before the mapWallet lock
        CTxDB txdb("r");
        {
            nFeeRet = nTransactionFee;
            loop
            {
                wtxNew.vin.clear();
                wtxNew.vout.clear();
                wtxNew.fFromMe = true;

                int64 nTotalValue = nValue + nFeeRet;
                printf("CreateTransactionWithInputTx: total value = %d\n", nTotalValue);
                double dPriority = 0;
                // vouts to the payees
                BOOST_FOREACH(const PAIRTYPE(CScript, int64)& s, vecSend)
                    wtxNew.vout.push_back(CTxOut(s.second, s.first));

                int64 nWtxinCredit = wtxIn.vout[nTxOut].nValue;

                // Choose coins to use
                set<pair<const CWalletTx*, unsigned int> > setCoins;
                int64 nValueIn = 0;
                printf("CreateTransactionWithInputTx: SelectCoins(%s), nTotalValue = %s, nWtxinCredit = %s\n", FormatMoney(nTotalValue - nWtxinCredit).c_str(), FormatMoney(nTotalValue).c_str(), FormatMoney(nWtxinCredit).c_str());
                if (nTotalValue - nWtxinCredit > 0)
                {
                    if (!pwalletMain->SelectCoins(nTotalValue - nWtxinCredit, wtxNew.nTime, wtxNew.nVersion, setCoins, nValueIn))
                        return false;
                }

                printf("CreateTransactionWithInputTx: selected %d tx outs, nValueIn = %s\n", setCoins.size(), FormatMoney(nValueIn).c_str());

                vector<pair<const CWalletTx*, unsigned int> > vecCoins(setCoins.begin(), setCoins.end());

                BOOST_FOREACH(PAIRTYPE(const CWalletTx*, unsigned int)& coin, vecCoins)
                {
                    int64 nCredit = coin.first->vout[coin.second].nValue;
                    dPriority += (double)nCredit * coin.first->GetDepthInMainChain();
                }

                // Input tx always at first position
                vecCoins.insert(vecCoins.begin(), make_pair(&wtxIn, nTxOut));

                nValueIn += nWtxinCredit;
                dPriority += (double)nWtxinCredit * wtxIn.GetDepthInMainChain();

                // Fill a vout back to self with any change
                int64 nChange = nValueIn - nTotalValue;
                if (nChange >= CENT)
                {
                    // Note: We use a new key here to keep it from being obvious which side is the change.
                    //  The drawback is that by not reusing a previous key, the change may be lost if a
                    //  backup is restored, if the backup doesn't have the new private key for the change.
                    //  If we reused the old key, it would be possible to add code to look for and
                    //  rediscover unknown transactions that were written with keys of ours to recover
                    //  post-backup change.

                    // Reserve a new key pair from key pool
                    CPubKey vchPubKey = reservekey.GetReservedKey();
                    assert(pwalletMain->HaveKey(vchPubKey.GetID()));

                    // -------------- Fill a vout to ourself, using same address type as the payment
                    // Now sending always to hash160 (GetBitcoinAddressHash160 will return hash160, even if pubkey is used)
                    CScript scriptChange;
                    //NOTE: look at bf798734db4539a39edd6badf54a1c3aecf193e5 commit in src/wallet.cpp
                    //if (vecSend[0].first.GetBitcoinAddressHash160() != 0)
                    //    scriptChange.SetBitcoinAddress(vchPubKey);
                    //else
                     //   scriptChange << vchPubKey << OP_CHECKSIG;
                    scriptChange.SetDestination(vchPubKey.GetID());

                    // Insert change txn at random position:
                    vector<CTxOut>::iterator position = wtxNew.vout.begin()+GetRandInt(wtxNew.vout.size());
                    wtxNew.vout.insert(position, CTxOut(nChange, scriptChange));
                }
                else
                    reservekey.ReturnKey();

                // Fill vin
                BOOST_FOREACH(PAIRTYPE(const CWalletTx*, unsigned int)& coin, vecCoins)
                    wtxNew.vin.push_back(CTxIn(coin.first->GetHash(), coin.second));

                // Sign
                int nIn = 0;
                BOOST_FOREACH(PAIRTYPE(const CWalletTx*, unsigned int)& coin, vecCoins)
                {
                    if (coin.first == &wtxIn && coin.second == nTxOut)
                    {
                        if (!SignNameSignature(*coin.first, wtxNew, nIn++))
                            throw runtime_error("could not sign name coin output");
                    }
                    else
                    {
                        if (!SignSignature(*pwalletMain, *coin.first, wtxNew, nIn++))
                            return false;
                    }
                }

                // Limit size
                unsigned int nBytes = ::GetSerializeSize(*(CTransaction*)&wtxNew, SER_NETWORK, PROTOCOL_VERSION);
                if (nBytes >= MAX_BLOCK_SIZE_GEN/5)
                    return false;
                dPriority /= nBytes;

                // Check that enough fee is included
                int64 nPayFee = nTransactionFee * (1 + (int64)nBytes / 1000);
                bool fAllowFree = CTransaction::AllowFree(dPriority);
                int64 nMinFee = wtxNew.GetMinFee(1, fAllowFree);
                if (nFeeRet < max(nPayFee, nMinFee))
                {
                    nFeeRet = max(nPayFee, nMinFee);
                    printf("CreateTransactionWithInputTx: re-iterating (nFreeRet = %s)\n", FormatMoney(nFeeRet).c_str());
                    continue;
                }

                // Fill vtxPrev by copying from previous transactions vtxPrev
                wtxNew.AddSupportingTransactions(txdb);
                wtxNew.fTimeReceivedIsTxTime = true;

                break;
            }
        }
    }
    printf("CreateTransactionWithInputTx succeeded:\n%s", wtxNew.ToString().c_str());
    return true;
}

// nTxOut is the output from wtxIn that we should grab
// requires cs_main lock
string SendMoneyWithInputTx(CScript scriptPubKey, int64 nValue, int64 nNetFee, CWalletTx& wtxIn, CWalletTx& wtxNew, bool fAskFee)
{
    int nTxOut = IndexOfNameOutput(wtxIn);
    CReserveKey reservekey(pwalletMain);
    int64 nFeeRequired;
    vector< pair<CScript, int64> > vecSend;
    vecSend.push_back(make_pair(scriptPubKey, nValue));

    if (nNetFee)
    {
        CScript scriptFee;
        scriptFee << OP_RETURN;
        vecSend.push_back(make_pair(scriptFee, nNetFee));
    }

    if (!CreateTransactionWithInputTx(vecSend, wtxIn, nTxOut, wtxNew, reservekey, nFeeRequired))
    {
        string strError;
        if (nValue + nFeeRequired > pwalletMain->GetBalance())
            strError = strprintf(_("Error: This transaction requires a transaction fee of at least %s because of its amount, complexity, or use of recently received funds "), FormatMoney(nFeeRequired).c_str());
        else
            strError = _("Error: Transaction creation failed  ");
        printf("SendMoney() : %s", strError.c_str());
        return strError;
    }

#ifdef GUI
    if (fAskFee && !uiInterface.ThreadSafeAskFee(nFeeRequired))
        return "ABORTED";
#else
    if (fAskFee && !ThreadSafeAskFee(nFeeRequired, "Emercoin"))
        return "ABORTED";
#endif

    if (!pwalletMain->CommitTransaction(wtxNew, reservekey))
        return _("SendMoneyWithInputTx(): The transaction was rejected.  This might happen if some of the coins in your wallet were already spent, such as if you used a copy of wallet.dat and coins were spent in the copy but not marked as spent here.");

    return "";
}

bool GetValueOfTxPos(const CNameIndex& txPos, vector<unsigned char>& vchValue, uint256& hash, int& nHeight)
{
    nHeight = GetTxPosHeight(txPos);
    vchValue = txPos.vValue;
    CTransaction tx;
    if (!tx.ReadFromDisk(txPos.txPos))
        return error("GetValueOfTxPos() : could not read tx from disk");
    hash = tx.GetHash();
    return true;
}

bool GetValueOfTxPos(const CDiskTxPos& txPos, vector<unsigned char>& vchValue, uint256& hash, int& nHeight)
{
    nHeight = GetTxPosHeight(txPos);
    CTransaction tx;
    if (!tx.ReadFromDisk(txPos))
        return error("GetValueOfTxPos() : could not read tx from disk");
    if (!GetValueOfNameTx(tx, vchValue))
        return error("GetValueOfTxPos() : could not decode value from tx");
    hash = tx.GetHash();
    return true;
}

// returns last known value for this name
bool GetNameValue(CNameDB& dbName, const vector<unsigned char>& vchName, vector<unsigned char>& vchValue)
{
    vector<CNameIndex> vtxPos;
    if (!dbName.ReadName(vchName, vtxPos) || vtxPos.empty())
        return false;

//    printf("GetNameValue() : vchName = %s -> (", stringFromVch(vchName).c_str());
//    BOOST_FOREACH(CNameIndex& itx, vtxPos)
//    {
//        printf("%s, ", stringFromVch(itx.vValue).c_str());
//    }
//    printf("), front = %s\n", stringFromVch(vtxPos.front().vValue).c_str());

    vchValue = vtxPos.back().vValue;
    return true;
}

bool CNameDB::ScanNames(
        vector<unsigned char> vchName,
        int nMax,
        vector<pair<vector<unsigned char>, CNameIndex> >& nameScan)
{
    //encode name before scanning
    encodeOrDecode(vchName);

    Dbc* pcursor = GetCursor();
    if (!pcursor)
        return false;

    unsigned int fFlags = DB_SET_RANGE;
    loop
    {
        // Read next record
        CDataStream ssKey(SER_DISK, CLIENT_VERSION);
        if (fFlags == DB_SET_RANGE)
            ssKey << make_pair(string("namei"), vchName);
        CDataStream ssValue(SER_DISK, CLIENT_VERSION);
        int ret = ReadAtCursor(pcursor, ssKey, ssValue, fFlags);
        fFlags = DB_NEXT;
        if (ret == DB_NOTFOUND)
            break;
        else if (ret != 0)
            return false;

        // Unserialize
        string strType;
        ssKey >> strType;
        if (strType == "namei")
        {
            vector<unsigned char> vchName;
            ssKey >> vchName;
            vector<CNameIndex> vtxPos;
            ssValue >> vtxPos;
            CNameIndex txPos;
            if (!vtxPos.empty())
            {
                txPos = vtxPos.back();
            }

            //decode returned data
            encodeOrDecode(vchName);
            encodeOrDecode(txPos.vValue);

            nameScan.push_back(make_pair(vchName, txPos));
        }

        if (nameScan.size() >= nMax)
            break;
    }
    pcursor->close();
    return true;
}

// this sould only be called in nameindex.dat does not exist
bool CNameDB::ReconstructNameIndex()
{
    CTxDB txdb("r");
    CTxIndex txindex;
    CBlockIndex* pindex = pindexGenesisBlock;
    {
        LOCK(pwalletMain->cs_wallet);
        while (pindex)
        {
            CBlock block;
            block.ReadFromDisk(pindex, true);

            BOOST_FOREACH(CTransaction& tx, block.vtx)
            {
                if (tx.nVersion != NAMECOIN_TX_VERSION)
                    continue;

                // posThisTx = txindex.pos
                if(!txdb.ReadTxIndex(tx.GetHash(), txindex))
                    return error("ReconstructNameIndex() : failed to read from disk, aborting.");

                // mapTestPool - is used in hooks->ConnectInput only if we have fMiner=true, so we don't care about it in this case
                map<uint256, CTxIndex> mapTestPool;
                MapPrevTx mapInputs;
                bool fInvalid;
                if (!tx.FetchInputs(txdb, mapTestPool, true, false, mapInputs, fInvalid))
                    return error("ReconstructNameIndex() : failed to read from disk, aborting.");

                // vTxPrev and vTxindex
                vector<CTxIndex> vTxindex;
                vector<CTransaction> vTxPrev;
                for (unsigned int i = 0; i < tx.vin.size(); i++)
                {
                    COutPoint prevout = tx.vin[i].prevout;
                    CTransaction& txPrev = mapInputs[prevout.hash].second;
                    CTxIndex& txindex = mapInputs[prevout.hash].first;

                    vTxPrev.push_back(txPrev);
                    vTxindex.push_back(txindex);
                }
                hooks->ConnectInputs(txdb, mapTestPool, tx, vTxPrev, vTxindex, pindex, txindex.pos, true, false);
            }
            pindex = pindex->pnext;
        }
    }
    return true;
}


CHooks* InitHook()
{
    return new CNamecoinHooks();
}

bool CNamecoinHooks::IsStandard(const CTransaction &tx)
{
    NameTxInfo nxi;
    if (!DecodeNameTx(tx, nxi))
        return false;
    return true;
}

bool checkNameValues(NameTxInfo& ret)
{
    ret.err_msg = "";
    if (ret.vchName.size() > MAX_NAME_LENGTH)
        ret.err_msg.append("name is too long.\n");

    if (ret.vchValue.size() > MAX_VALUE_LENGTH)
        ret.err_msg.append("value is too long.\n");

    if (ret.op == OP_NAME_NEW && ret.nRentalDays < 1)
        ret.err_msg.append("rental days must be greater than 0.\n");

    if (ret.op == OP_NAME_UPDATE && ret.nRentalDays < 0)
        ret.err_msg.append("rental days must be greater or equal 0.\n");

    if (ret.nRentalDays > MAX_RENTAL_DAYS)
        ret.err_msg.append("rental days value is too large.\n");

    if (ret.err_msg != "")
        return false;
    return true;
}

// read name script and extract: name, value and rentalDays
// optionaly it can extract destination address and check if tx is mine (note: it does not check if address is valid)
bool DecodeNameScript(const CScript& script, NameTxInfo &ret, bool checkValuesCorrectness, bool checkAddressAndIfIsMine)
{
    CScript::const_iterator pc = script.begin();
    return DecodeNameScript(script, ret, pc, checkValuesCorrectness, checkAddressAndIfIsMine);
}

bool DecodeNameScript(const CScript& script, NameTxInfo& ret, CScript::const_iterator& pc, bool checkValuesCorrectness, bool checkAddressAndIfIsMine)
{
    // script structure:
    // name_op << name << days << OP_2DROP << OP_DROP << val1 << val2 << .. << valn << OP_DROP2 << OP_DROP2 << ..<< OP_DROP2|OP_DROP << paytoscripthash

    // NOTE: script structure is strict - it must not contain anything else to be a valid name script.

    ret.nOut = -1;       //CScript does not have nOut

    // read op
    ret.err_msg = "failed to read op";
    opcodetype opcode;
    if (!script.GetOp(pc, opcode))
        return false;
    if (opcode < OP_1 || opcode > OP_16)
        return false;
    ret.op = opcode - OP_1 + 1;

    vector<unsigned char> vch;

    // read name
    ret.err_msg = "failed to read name";
    if (!script.GetOp(pc, opcode, vch))
        return false;
    if ((opcode == OP_DROP || opcode == OP_2DROP) || !(opcode >= 0 && opcode <= OP_PUSHDATA4))
        return false;
    ret.vchName = vch;

    // read rental days
    ret.err_msg = "failed to read rental days";
    if (!script.GetOp(pc, opcode, vch))
        return false;
    if ((opcode == OP_DROP || opcode == OP_2DROP) || !(opcode >= 0 && opcode <= OP_PUSHDATA4))
        return false;
    ret.nRentalDays = CBigNum(vch).getint();

    // read two delimiters -  OP_2DROP, OP_DROP
    ret.err_msg = "failed to read delimeter d in: name << rental << d << value";
    if (!script.GetOp(pc, opcode, vch))
        return false;
    if (opcode != OP_2DROP)
        return false;
    if (!script.GetOp(pc, opcode, vch))
        return false;
    if (opcode != OP_DROP)
        return false;

    // read value
    ret.err_msg = "failed to read value";
    int valueSize = 0;
    for (;;)
    {
        if (!script.GetOp(pc, opcode, vch))
            return false;
        if (opcode == OP_DROP || opcode == OP_2DROP)
            break;
        if (!(opcode >= 0 && opcode <= OP_PUSHDATA4))
            return false;
        ret.vchValue.insert(ret.vchValue.end(), vch.begin(), vch.end());
        valueSize++;
    }
    pc--;

    // read next delimiter and move the pc after it
    ret.err_msg = "failed to read correct number of DROP operations after value"; //sucess! we have read name script structure
    int delimiterSize = 0;
    while (opcode == OP_DROP || opcode == OP_2DROP)
    {
        if (!script.GetOp(pc, opcode))
            break;
        if (opcode == OP_2DROP)
            delimiterSize += 2;
        if (opcode == OP_DROP)
            delimiterSize += 1;
    }
    pc--;

    if (valueSize != delimiterSize)
        return false;

    //decode xored data
    encodeOrDecode(ret.vchName);
    encodeOrDecode(ret.vchValue);

    //sucess! we have read name script structure without errors!
    ret.err_msg = "";

    if (checkValuesCorrectness)
    {
        if (!checkNameValues(ret))
            return false;
    }

    if (checkAddressAndIfIsMine)
    {
        //read address
        CTxDestination address;
        CScript scriptPubKey(pc, script.end());
        if (!ExtractDestination(scriptPubKey, address))
            ret.strAddress = "";
        else ret.strAddress = CBitcoinAddress(address).ToString();

        // check if this is mine
        CScript scriptSig;
        txnouttype whichType;
        if (!Solver(*pwalletMain, scriptPubKey, 0, 0, scriptSig, whichType))
            ret.nIsMine = 0;
        else ret.nIsMine = 1;
    }

    return true;
}

//returns first name operation. I.e. name_new from chain like name_new->name_update->name_update->...->name_update
//note: if name expire then such chain is deleted and new chain is started when new name_new is issued. So, only a single name_new can associated with a name at any given moment.
bool GetFirstTxOfName(CNameDB& dbName, const vector<unsigned char> &vchName, CTransaction& tx)
{
    vector<CNameIndex> vtxPos;
    if (!dbName.ReadName(vchName, vtxPos) || vtxPos.empty())
        return false;
    CNameIndex& txPos = vtxPos.front();
    int nHeight = txPos.nHeight;

    int nTotalLifeTime;
    if (!GetNameTotalLifeTime(vchName, nTotalLifeTime))
        return false;

    if (nHeight + nTotalLifeTime < pindexBest->nHeight)
    {
        printf("GetFirstTxOfName(%s) : expired", stringFromVch(vchName).c_str());
        return false;
    }

    if (!tx.ReadFromDisk(txPos.txPos))
        return error("GetFirstTxOfName() : could not read tx from disk");
    return true;
}

bool GetLastTxOfName(CNameDB& dbName, const vector<unsigned char> &vchName, CTransaction& tx)
{
    vector<CNameIndex> vtxPos;
    if (!dbName.ReadName(vchName, vtxPos) || vtxPos.empty())
        return false;
    CNameIndex& txPos = vtxPos.back();
    int nHeight = txPos.nHeight;

    int nTotalLifeTime;
    if (!GetNameTotalLifeTime(vchName, nTotalLifeTime))
        return false;

    if (nHeight + nTotalLifeTime < pindexBest->nHeight)
    {
        printf("GetFirstTxOfName(%s) : expired", stringFromVch(vchName).c_str());
        return false;
    }

    if (!tx.ReadFromDisk(txPos.txPos))
        return error("GetFirstTxOfName() : could not read tx from disk");
    return true;
}


//Value sendtoname(const Array& params, bool fHelp)
//{
//    if (fHelp || params.size() < 2 || params.size() > 4)
//        throw runtime_error(
//            "sendtoname <namecoinname> <amount> [comment] [comment-to]\n"
//            "<amount> is a real and is rounded to the nearest 0.01"
//            + HelpRequiringPassphrase());

//    vector<unsigned char> vchName = vchFromValue(params[0]);
//    CNameDB dbName("r");
//    if (!dbName.ExistsName(vchName))
//        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Name not found");

//    string strAddress;
//    CTransaction tx;
//    GetFirstTxOfName(dbName, vchName, tx);
//    GetNameAddress(tx, strAddress);

//    uint160 hash160;
//    if (!AddressToHash160(strAddress, hash160))
//        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "No valid namecoin address");

//    // Amount
//    int64 nAmount = AmountFromValue(params[1]);

//    // Wallet comments
//    CWalletTx wtx;
//    if (params.size() > 2 && params[2].type() != null_type && !params[2].get_str().empty())
//        wtx.mapValue["comment"] = params[2].get_str();
//    if (params.size() > 3 && params[3].type() != null_type && !params[3].get_str().empty())
//        wtx.mapValue["to"]      = params[3].get_str();

//    {
//        LOCK(cs_main);
//        EnsureWalletIsUnlocked();

//        string strError = pwalletMain->SendMoneyToBitcoinAddress(strAddress, nAmount, wtx);
//        if (strError != "")
//            throw JSONRPCError(RPC_WALLET_ERROR, strError);
//    }

//    return wtx.GetHash().GetHex();
//}

bool IsMyName(const CTxOut& txout)
{
    CScript scriptPubKey = RemoveNameScriptPrefix(txout.scriptPubKey);
    CScript scriptSig;
    txnouttype whichType;
    if (!Solver(*pwalletMain, scriptPubKey, 0, 0, scriptSig, whichType))
        return false;
    return true;
}

bool CNamecoinHooks::IsMine(const CTransaction& tx)
{
    NameTxInfo nti;
    if (!DecodeNameTx(tx, nti))
        return false;

    return IsMyName(tx.vout[nti.nOut]);
}

bool CNamecoinHooks::IsMine(const CTransaction& tx, const CTxOut& txout)
{
    //check given txout only if this a valid namecoin tx
    NameTxInfo nti;
    if (!DecodeNameTx(tx, nti))
        return false;

    return IsMyName(txout);
}


Value name_list(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 1)
        throw runtime_error(
                "name_list [<name>]\n"
                "list my own names"
                );

    vector<unsigned char> vchNameUniq;
    if (params.size() == 1)
        vchNameUniq = vchFromValue(params[0]);

    map<vector<unsigned char>, NameTxInfo> scannedNames = GetNameList(vchNameUniq);

    Array oRes;
    BOOST_FOREACH(const PAIRTYPE(vector<unsigned char>, NameTxInfo)& item, scannedNames)
    {
        Object oName;
        oName.push_back(Pair("name", stringFromVch(item.second.vchName)));
        oName.push_back(Pair("value", stringFromVch(item.second.vchValue)));
        if (item.second.nIsMine == 0)
            oName.push_back(Pair("transferred", 1));
        oName.push_back(Pair("address", item.second.strAddress));
        oName.push_back(Pair("expires_in", item.second.nTimeLeft));
        if (item.second.nTimeLeft <= 0)
            oName.push_back(Pair("expired", 1));

        oRes.push_back(oName);
    }
    return oRes;
}

//read wallet txs and extract: name, value, rentalDays, nOut and timeLeft (and TODO: address)
map<vector<unsigned char>, NameTxInfo> GetNameList(const vector<unsigned char> &vchNameUniq)
{
    map<vector<unsigned char>, NameTxInfo> scannedNames;
    {
        LOCK(pwalletMain->cs_wallet);
        CTxDB txdb("r");
        CNameDB dbName("r");

        //add all names from wallet tx
        map< vector<unsigned char>, int > mapHeight;
        BOOST_FOREACH(PAIRTYPE(const uint256, CWalletTx)& item, pwalletMain->mapWallet)
        {
            CTransaction tx;
            CTxIndex txindex;
            if(!txdb.ReadDiskTx(item.second.GetHash(), tx, txindex))
                continue;

            NameTxInfo nti;
            if (!DecodeNameTx(tx, nti, false, true))
                continue;

            if (!dbName.ExistsName(nti.vchName))
                continue;

            // allow only latest wallet tx with that name
            int thisTxDepth = txindex.GetDepthInMainChain();
            if (mapHeight.count(nti.vchName) == 1 && mapHeight[nti.vchName] < thisTxDepth)
                continue;

            if (vchNameUniq.size() > 0 && vchNameUniq != nti.vchName)
                continue;

            if (!GetNameValue(dbName, nti.vchName, nti.vchValue))   // get last known value
                continue;

            int nTotalLifeTime, nNameHeight;
            if (!GetExpirationData(dbName, nti.vchName, nTotalLifeTime, nNameHeight))
                continue;
            nti.nTimeLeft = (nTotalLifeTime + nNameHeight) - pindexBest->nHeight;

            scannedNames[nti.vchName] = nti;
            mapHeight[nti.vchName] = thisTxDepth;
        }
    }
    return scannedNames;
}

Value name_debug(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 1)
        throw runtime_error(
            "name_debug\n"
            "Dump pending transactions id in the debug file.\n");

    printf("Pending:\n----------------------------\n");
    pair<vector<unsigned char>, set<uint256> > pairPending;

    {
        LOCK(cs_main);
        BOOST_FOREACH(pairPending, mapNamePending)
        {
            string name = stringFromVch(pairPending.first);
            printf("%s :\n", name.c_str());
            uint256 hash;
            BOOST_FOREACH(hash, pairPending.second)
            {
                printf("    ");
                if (!pwalletMain->mapWallet.count(hash))
                    printf("foreign ");
                printf("    %s\n", hash.GetHex().c_str());
            }
        }
    }
    printf("----------------------------\n");
    return true;
}

Value name_debug1(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 1)
        throw runtime_error(
            "name_debug1 <name>\n"
            "Dump name blocks number and transactions id in the debug file.\n");

    vector<unsigned char> vchName = vchFromValue(params[0]);
    printf("Dump name:\n");
    {
        LOCK(cs_main);
        //vector<CDiskTxPos> vtxPos;
        vector<CNameIndex> vtxPos;
        CNameDB dbName("r");
        if (!dbName.ReadName(vchName, vtxPos))
        {
            error("failed to read from name DB");
            return false;
        }
        //CDiskTxPos txPos;
        CNameIndex txPos;
        BOOST_FOREACH(txPos, vtxPos)
        {
            CTransaction tx;
            if (!tx.ReadFromDisk(txPos.txPos))
            {
                error("could not read txpos %s", txPos.txPos.ToString().c_str());
                continue;
            }
            printf("@%d %s\n", GetTxPosHeight(txPos), tx.GetHash().GetHex().c_str());
        }
    }
    printf("-------------------------\n");
    return true;
}

//TODO: name_show, name_history, sendtoname

//Value name_show(const Array& params, bool fHelp)
//{
//    if (fHelp || params.size() != 1)
//        throw runtime_error(
//            "name_show <name>\n"
//            "Show values of a name.\n"
//            );

//    Object oLastName;
//    vector<unsigned char> vchName = vchFromValue(params[0]);
//    string name = stringFromVch(vchName);
//    {
//        LOCK(cs_main);
//        //vector<CDiskTxPos> vtxPos;
//        vector<CNameIndex> vtxPos;
//        CNameDB dbName("r");
//        if (!dbName.ReadName(vchName, vtxPos))
//            throw JSONRPCError(RPC_WALLET_ERROR, "failed to read from name DB");

//        if (vtxPos.size() < 1)
//            throw JSONRPCError(RPC_WALLET_ERROR, "no result returned");

//        CDiskTxPos txPos = vtxPos[vtxPos.size() - 1].txPos;
//        CTransaction tx;
//        if (!tx.ReadFromDisk(txPos))
//            throw JSONRPCError(RPC_WALLET_ERROR, "failed to read from from disk");

//        Object oName;
//        vector<unsigned char> vchValue;
//        int nHeight;
//        uint256 hash;
//        if (!txPos.IsNull() && GetValueOfTxPos(txPos, vchValue, hash, nHeight))
//        {
//            oName.push_back(Pair("name", name));
//            string value = stringFromVch(vchValue);
//            oName.push_back(Pair("value", value));
//            oName.push_back(Pair("txid", tx.GetHash().GetHex()));
//            string strAddress = "";
//            GetNameAddress(txPos, strAddress);
//            oName.push_back(Pair("address", strAddress));
//            oName.push_back(Pair("expires_in", nHeight + GetDisplayExpirationDepth(nHeight) - pindexBest->nHeight));
//            if(nHeight + GetDisplayExpirationDepth(nHeight) - pindexBest->nHeight <= 0)
//            {
//                oName.push_back(Pair("expired", 1));
//            }
//            oLastName = oName;
//        }
//    }
//    return oLastName;
//}

//Value name_history(const Array& params, bool fHelp)
//{
//    if (fHelp || params.size() != 1)
//        throw runtime_error(
//            "name_history <name>\n"
//            "List all name values of a name.\n");

//    Array oRes;
//    vector<unsigned char> vchName = vchFromValue(params[0]);
//    string name = stringFromVch(vchName);
//    {
//        LOCK(cs_main);
//        //vector<CDiskTxPos> vtxPos;
//        vector<CNameIndex> vtxPos;
//        CNameDB dbName("r");
//        if (!dbName.ReadName(vchName, vtxPos))
//            throw JSONRPCError(RPC_WALLET_ERROR, "failed to read from name DB");

//        CNameIndex txPos2;
//        CDiskTxPos txPos;
//        BOOST_FOREACH(txPos2, vtxPos)
//        {
//            txPos = txPos2.txPos;
//            CTransaction tx;
//            if (!tx.ReadFromDisk(txPos))
//            {
//                error("could not read txpos %s", txPos.ToString().c_str());
//                continue;
//            }

//            Object oName;
//            vector<unsigned char> vchValue;
//            int nHeight;
//            uint256 hash;
//            if (!txPos.IsNull() && GetValueOfTxPos(txPos, vchValue, hash, nHeight))
//            {
//                oName.push_back(Pair("name", name));
//                string value = stringFromVch(vchValue);
//                oName.push_back(Pair("value", value));
//                oName.push_back(Pair("txid", tx.GetHash().GetHex()));
//                string strAddress = "";
//                GetNameAddress(txPos, strAddress);
//                oName.push_back(Pair("address", strAddress));
//                oName.push_back(Pair("expires_in", nHeight + GetDisplayExpirationDepth(nHeight) - pindexBest->nHeight));
//                if(nHeight + GetDisplayExpirationDepth(nHeight) - pindexBest->nHeight <= 0)
//                {
//                    oName.push_back(Pair("expired", 1));
//                }
//                oRes.push_back(oName);
//            }
//        }
//    }
//    return oRes;
//}

Value name_filter(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 5)
        throw runtime_error(
                "name_filter [[[[[regexp] maxage=36000] from=0] nb=0] stat]\n"
                "scan and filter names\n"
                "[regexp] : apply [regexp] on names, empty means all names\n"
                "[maxage] : look in last [maxage] blocks\n"
                "[from] : show results from number [from]\n"
                "[nb] : show [nb] results, 0 means all\n"
                "[stats] : show some stats instead of results\n"
                "name_filter \"\" 5 # list names updated in last 5 blocks\n"
                "name_filter \"^id/\" # list all names from the \"id\" namespace\n"
                "name_filter \"^id/\" 36000 0 0 stat # display stats (number of names) on active names from the \"id\" namespace\n"
                );

    string strRegexp;
    int nFrom = 0;
    int nNb = 0;
    int nMaxAge = 36000;
    bool fStat = false;
    int nCountFrom = 0;
    int nCountNb = 0;


    if (params.size() > 0)
        strRegexp = params[0].get_str();

    if (params.size() > 1)
        nMaxAge = params[1].get_int();

    if (params.size() > 2)
        nFrom = params[2].get_int();

    if (params.size() > 3)
        nNb = params[3].get_int();

    if (params.size() > 4)
        fStat = (params[4].get_str() == "stat" ? true : false);


    CNameDB dbName("r");
    Array oRes;

    vector<unsigned char> vchName;
    vector<pair<vector<unsigned char>, CNameIndex> > nameScan;
    if (!dbName.ScanNames(vchName, 100000000, nameScan))
        throw JSONRPCError(RPC_WALLET_ERROR, "scan failed");

    // compile regex once
    using namespace boost::xpressive;
    smatch nameparts;
    sregex cregex = sregex::compile(strRegexp);

    pair<vector<unsigned char>, CNameIndex> pairScan;
    BOOST_FOREACH(pairScan, nameScan)
    {
        string name = stringFromVch(pairScan.first);

        // regexp
        if(strRegexp != "" && !regex_search(name, nameparts, cregex))
            continue;

        CNameIndex txName = pairScan.second;

        int nTotalLifeTime, nHeight;
        if (!GetExpirationData(vchName, nTotalLifeTime, nHeight))
            continue;

        // max age
        if(nMaxAge != 0 && pindexBest->nHeight - nHeight >= nMaxAge)
            continue;

        // from limits
        nCountFrom++;
        if(nCountFrom < nFrom + 1)
            continue;

        Object oName;
        if (!fStat) {
            oName.push_back(Pair("name", name));

            int nExpiresIn = nHeight + nTotalLifeTime - pindexBest->nHeight;
            if (nExpiresIn <= 0)
            {
                oName.push_back(Pair("expired", 1));
            }
            else
            {
                string value = stringFromVch(txName.vValue);
                oName.push_back(Pair("value", value));
                oName.push_back(Pair("expires_in", nExpiresIn));
            }
        }
        oRes.push_back(oName);

        nCountNb++;
        // nb limits
        if(nNb > 0 && nCountNb >= nNb)
            break;
    }

    if (fStat)
    {
        Object oStat;
        oStat.push_back(Pair("blocks",    (int)nBestHeight));
        oStat.push_back(Pair("count",     (int)oRes.size()));
        //oStat.push_back(Pair("sha256sum", SHA256(oRes), true));
        return oStat;
    }

    return oRes;
}

Value name_scan(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 2)
        throw runtime_error(
                "name_scan [<start-name>] [<max-returned>]\n"
                "scan all names, starting at start-name and returning a maximum number of entries (default 500)\n"
                );

    vector<unsigned char> vchName;
    int nMax = 500;
    if (params.size() > 0)
    {
        vchName = vchFromValue(params[0]);
    }

    if (params.size() > 1)
    {
        Value vMax = params[1];
        ConvertTo<double>(vMax);
        nMax = (int)vMax.get_real();
    }

    CNameDB dbName("r");
    Array oRes;

    //vector<pair<vector<unsigned char>, CDiskTxPos> > nameScan;
    vector<pair<vector<unsigned char>, CNameIndex> > nameScan;
    if (!dbName.ScanNames(vchName, nMax, nameScan))
        throw JSONRPCError(RPC_WALLET_ERROR, "scan failed");

    //pair<vector<unsigned char>, CDiskTxPos> pairScan;
    pair<vector<unsigned char>, CNameIndex> pairScan;
    BOOST_FOREACH(pairScan, nameScan)
    {
        Object oName;
        string name = stringFromVch(pairScan.first);
        oName.push_back(Pair("name", name));

        CTransaction tx;
        CNameIndex txName = pairScan.second;
        CDiskTxPos txPos = txName.txPos;

        vector<unsigned char> vchValue = txName.vValue;

        int nTotalLifeTime, nHeight;
        if (!GetExpirationData(dbName, pairScan.first, nTotalLifeTime, nHeight))
            continue;

        if ((nHeight + nTotalLifeTime - pindexBest->nHeight <= 0)
            || txPos.IsNull()
            || !tx.ReadFromDisk(txPos))
            //|| !GetValueOfNameTx(tx, vchValue))
        {
            oName.push_back(Pair("expired", 1));
        }
        else
        {
            string value = stringFromVch(vchValue);
            //string strAddress = "";
            //GetNameAddress(tx, strAddress);
            oName.push_back(Pair("value", value));
            //oName.push_back(Pair("txid", tx.GetHash().GetHex()));
            //oName.push_back(Pair("address", strAddress));
            oName.push_back(Pair("expires_in", nHeight + nTotalLifeTime - pindexBest->nHeight));
        }
        oRes.push_back(oName);
    }
    return oRes;
}

//xor data, so that antivirus will not complain about database files (if there is a virus)
void encodeOrDecode(vector<unsigned char> &vchData)
{
    if (vchData.size() == 0)
        return;

    vector<unsigned char> vchKey = vchFromString("Emergence is inevitable!");

    for (int i = 0, j = 0; i < vchData.size(); i++, j++)
    {
        j %= vchKey.size();   //reset key iterator if it is out of bounds
        vchData[i] ^= vchKey[j];
    }
}

bool createNameScript(CScript& nameScript, vector<unsigned char> vchName, vector<unsigned char> vchValue, int nRentalDays, int op, string& err_msg)
{
    {
        NameTxInfo nti(vchName, vchValue, nRentalDays, op, -1, err_msg);
        if (!checkNameValues(nti))
        {
            err_msg = nti.err_msg;
            return false;
        }
    }

    //encode data, so that antivirus will complain less (if it contains virus)
    encodeOrDecode(vchName);
    encodeOrDecode(vchValue);

    vector<unsigned char> vchRentalDays = CBigNum(nRentalDays).getvch();

    //add name and rental days
    nameScript << op << vchName << vchRentalDays << OP_2DROP << OP_DROP;

    // split value in 520 bytes chunks and add it to script
    {
        int nChunks = ceil(vchValue.size() / 520.0);

        for (unsigned int i = 0; i < nChunks; i++)
        {   // insert data
            vector<unsigned char>::const_iterator sliceBegin = vchValue.begin() + i*520;
            vector<unsigned char>::const_iterator sliceEnd = min(vchValue.begin() + (i+1)*520, vchValue.end());
            vector<unsigned char> vchSubValue(sliceBegin, sliceEnd);
            nameScript << vchSubValue;
        }

            //insert end markers
        for (unsigned int i = 0; i < nChunks / 2; i++)
            nameScript << OP_2DROP;
        if (nChunks % 2 != 0)
            nameScript << OP_DROP;
    }
    return true;
}

Value name_new(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 3)
        throw runtime_error(
                "name_new <name> <value> <days>\n"
                "Creates new key->value pair which expires after specified number of days.\n"
                "Cost is square root of (1% of last PoW + 1% per year of last PoW)."
                + HelpRequiringPassphrase());
    vector<unsigned char> vchName = vchFromValue(params[0]);
    vector<unsigned char> vchValue = vchFromValue(params[1]);
    int nRentalDays = params[2].get_int();

//    CScript nameScript;
//    string err_msg = "";
//    int op = 666;
//    if (!createNameScript(nameScript, vchName, vchValue, nRentalDays, op, err_msg))
//        throw runtime_error(err_msg);
//    printf("NameScript:\n%s\n", nameScript.ToString().c_str());

//    NameScriptInfo txInfo;
//    if (!decodeNameScript(nameScript, txInfo, err_msg))
//        throw runtime_error(err_msg);

    NameTxReturn ret = name_new(vchName, vchValue, nRentalDays);
    if (!ret.ok)
        throw JSONRPCError(ret.err_code, ret.err_msg);
    return ret.hex.GetHex();
}

NameTxReturn name_new(const vector<unsigned char> &vchName,
              const vector<unsigned char> &vchValue,
              const int nRentalDays)
{
    NameTxReturn ret;
    ret.err_code = RPC_INTERNAL_ERROR; //default value
    ret.ok = false;

    CWalletTx wtx;
    wtx.nVersion = NAMECOIN_TX_VERSION;
    stringstream ss;
    CScript scriptPubKey;

    {
        LOCK(cs_main);

        if (mapNamePending.count(vchName) && mapNamePending[vchName].size())
        {
            ss << "there are " << mapNamePending[vchName].size() <<
                  " pending operations on that name, including " <<
                  mapNamePending[vchName].begin()->GetHex().c_str();
            ret.err_msg = ss.str();
            return ret;
        }

        CNameDB dbName("r");
        CTransaction tx;
        if (GetFirstTxOfName(dbName, vchName, tx))
        {
            ss << "this name is already active with tx " << mapNamePending[vchName].begin()->GetHex().c_str();
            ret.err_msg = ss.str();
            return ret;
        }

        EnsureWalletIsUnlocked();

        CPubKey vchPubKey;
        if (!pwalletMain->GetKeyFromPool(vchPubKey, true))
        {
            ret.err_msg = "failed to get key from pool";
            return ret;
        }
        scriptPubKey.SetDestination(vchPubKey.GetID());

        CScript nameScript;
        if (!createNameScript(nameScript, vchName, vchValue, nRentalDays, OP_NAME_NEW, ret.err_msg))
            return ret;

        nameScript += scriptPubKey;

        int64 prevFee = nTransactionFee;
        nTransactionFee = GetNameNewFee(pindexBest, nRentalDays);
        string strError = pwalletMain->SendMoney(nameScript, CENT, wtx, false);
        nTransactionFee = prevFee;
        if (strError != "")
        {
            ret.err_code = RPC_WALLET_ERROR;
            ret.err_msg = strError;
            return ret;
        }
    }

    //success! collect info and return
    CTxDestination address;
    if (ExtractDestination(scriptPubKey, address))
    {
        ret.address = CBitcoinAddress(address).ToString();
    }
    ret.hex = wtx.GetHash();
    ret.ok = true;
    return ret;
}

Value name_update(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 2 || params.size() > 4)
        throw runtime_error(
                "name_update <name> <value> <days> [<toaddress>]\nUpdate name value, add days to expiration time and possibly transfer a name to diffrent address."
                + HelpRequiringPassphrase());

    vector<unsigned char> vchName = vchFromValue(params[0]);
    vector<unsigned char> vchValue = vchFromValue(params[1]);
    int nRentalDays = params[2].get_int();
    string strAddress = "";
    if (params.size() == 4)
        strAddress = params[3].get_str();
    printf("vchValue.size = %d\n", vchValue.size());

    NameTxReturn ret = name_update(vchName, vchValue, nRentalDays, strAddress);
    if (!ret.ok)
        throw JSONRPCError(ret.err_code, ret.err_msg);
    return ret.hex.GetHex();
}

NameTxReturn name_update(const vector<unsigned char> &vchName,
              const vector<unsigned char> &vchValue,
              const int nRentalDays,
              string strAddress)
{
    NameTxReturn ret;
    ret.err_code = RPC_INTERNAL_ERROR; //default value
    ret.ok = false;

    CWalletTx wtx;
    wtx.nVersion = NAMECOIN_TX_VERSION;
    stringstream ss;
    CScript scriptPubKey;

    {
    //3 checks - pending operations, name exist?, name is yours?
        LOCK2(cs_main, pwalletMain->cs_wallet);

        if (mapNamePending.count(vchName) && mapNamePending[vchName].size())
        {
            ss << "there are " << mapNamePending[vchName].size() <<
                  " pending operations on that name, including " <<
                  mapNamePending[vchName].begin()->GetHex().c_str();
            ret.err_msg = ss.str();
            return ret;
        }

        CNameDB dbName("r");
        CTransaction tx; //we need to select last input
        if (!GetLastTxOfName(dbName, vchName, tx))
        {
            ret.err_msg = "could not find a coin with this name";
            return ret;
        }

        uint256 wtxInHash = tx.GetHash();
        if (!pwalletMain->mapWallet.count(wtxInHash))
        {
            ss << "this coin is not in your wallet " << wtxInHash.GetHex().c_str();
            ret.err_msg = ss.str();
            return ret;
        }
        else
        {
            CWalletTx& wtxIn = pwalletMain->mapWallet[wtxInHash];
            int nTxOut = IndexOfNameOutput(wtxIn);
            if (wtxIn.IsSpent(nTxOut))
            {
                ret.err_msg = "Last tx of this name was spent by non-namecoin tx. This means that this name cannot be updated anymore - you will have to wait until it expires.";
                return ret;
            }
        }

    //form script and send
        if (strAddress != "")
        {
            CBitcoinAddress address(strAddress);
            if (!address.IsValid())
            {
                ret.err_code = RPC_INVALID_ADDRESS_OR_KEY;
                ret.err_msg = "could not find a coin with this name";
                return ret;
            }
            scriptPubKey.SetDestination(address.Get());
        }
        else
        {
            CPubKey vchPubKey;
            if(!pwalletMain->GetKeyFromPool(vchPubKey, true))
            {
                ret.err_msg = "failed to get key from pool";
                return ret;
            }
            scriptPubKey.SetDestination(vchPubKey.GetID());
        }

        CScript nameScript;
        if (!createNameScript(nameScript, vchName, vchValue, nRentalDays, OP_NAME_UPDATE, ret.err_msg))
            return ret;

        nameScript += scriptPubKey;

        EnsureWalletIsUnlocked();

        CWalletTx& wtxIn = pwalletMain->mapWallet[wtxInHash];

        int64 prevFee = nTransactionFee;
        nTransactionFee = GetNameUpdateFee(pindexBest, nRentalDays);
        string strError = SendMoneyWithInputTx(nameScript, CENT, 0, wtxIn, wtx, false);
        nTransactionFee = prevFee;
        if (strError != "")
            throw JSONRPCError(RPC_WALLET_ERROR, strError);

        if (strError != "")
        {
            ret.err_code = RPC_WALLET_ERROR;
            ret.err_msg = strError;
            return ret;
        }
    }

    //success! collect info and return
    CTxDestination address;
    ret.address = "";
    if (ExtractDestination(scriptPubKey, address))
    {
        ret.address = CBitcoinAddress(address).ToString();
    }
    ret.hex = wtx.GetHash();
    ret.ok = true;
    return ret;
}

void rescanfornames()
{
    printf("Scanning blockchain for names to create fast index...\n");

    CNameDB dbName("cr+");

    // scan blockchain
    dbName.ReconstructNameIndex();
}

//bool IsConflictedTx(CTxDB& txdb, const CTransaction& tx, vector<unsigned char>& name)
//{
//    if (tx.nVersion != NAMECOIN_TX_VERSION)
//        return false;
//    vector<vector<unsigned char> > vvchArgs;
//    int op;
//    int nOut;

//    bool good = DecodeNameTx(tx, op, nOut, vvchArgs);
//    if (!good)
//        return error("IsConflictedTx() : could not decode a namecoin tx");

//    switch (op)
//    {
//        case OP_NAME_NEW:
//            int nPrevHeight = GetNameHeight(txdb, vvchArgs[0]);
//            name = vvchArgs[0];
//            int nTotalLifeTime;
//            if (!GetNameTotalLifeTime(name, nTotalLifeTime))
//                return false;
//            if (nPrevHeight >= 0 && pindexBest->nHeight - nPrevHeight < nTotalLifeTime)
//                return true;
//    }
//    return false;
//}

//TODO: study if this is correct with new code. Re-add this command later.
//Value name_clean(const Array& params, bool fHelp)
//{
//    if (fHelp || params.size())
//        throw runtime_error("name_clean\nClean unsatisfiable transactions from the wallet - including name_update on an already taken name\n");

//    {
//        LOCK2(cs_main, pwalletMain->cs_wallet);
//        map<uint256, CWalletTx> mapRemove;

//        printf("-----------------------------\n");

//        {
//            CTxDB txdb("r");
//            BOOST_FOREACH(PAIRTYPE(const uint256, CWalletTx)& item, pwalletMain->mapWallet)
//            {
//                CWalletTx& wtx = item.second;
//                vector<unsigned char> vchName;
//                if (wtx.GetDepthInMainChain() < 1 && IsConflictedTx(txdb, wtx, vchName))
//                {
//                    uint256 hash = wtx.GetHash();
//                    mapRemove[hash] = wtx;
//                }
//            }
//        }

//        bool fRepeat = true;
//        while (fRepeat)
//        {
//            fRepeat = false;
//            BOOST_FOREACH(PAIRTYPE(const uint256, CWalletTx)& item, pwalletMain->mapWallet)
//            {
//                CWalletTx& wtx = item.second;
//                BOOST_FOREACH(const CTxIn& txin, wtx.vin)
//                {
//                    uint256 hash = wtx.GetHash();

//                    // If this tx depends on a tx to be removed, remove it too
//                    if (mapRemove.count(txin.prevout.hash) && !mapRemove.count(hash))
//                    {
//                        mapRemove[hash] = wtx;
//                        fRepeat = true;
//                    }
//                }
//            }
//        }

//        BOOST_FOREACH(PAIRTYPE(const uint256, CWalletTx)& item, mapRemove)
//        {
//            CWalletTx& wtx = item.second;

//            UnspendInputs(wtx);
//            mempool.remove(wtx);
//            pwalletMain->EraseFromWallet(wtx.GetHash());
//            vector<unsigned char> vchName;
//            if (GetNameOfTx(wtx, vchName) && mapNamePending.count(vchName))
//            {
//                string name = stringFromVch(vchName);
//                printf("name_clean() : erase %s from pending of name %s",
//                        wtx.GetHash().GetHex().c_str(), name.c_str());
//                if (!mapNamePending[vchName].erase(wtx.GetHash()))
//                    error("name_clean() : erase but it was not pending");
//            }
//            wtx.print();
//        }

//        printf("-----------------------------\n");
//    }

//    return true;
//}

// Check that the last entry in name history matches the given tx pos
bool CheckNameTxPos(const vector<CNameIndex> &vtxPos, const CDiskTxPos& txPos)
{
    if (vtxPos.empty())
        return false;

    return vtxPos.back().txPos == txPos;
}

// read name tx and extract: name, value and rentalDays
// optionaly it can extract destination address and check if tx is mine (note: it does not check if address is valid)
bool DecodeNameTx(const CTransaction& tx, NameTxInfo& nti, bool checkValuesCorrectness, bool checkAddressAndIfIsMine)
{
    if (tx.nVersion != NAMECOIN_TX_VERSION)
        return false;

    bool found = false;
    for (int i = 0; i < tx.vout.size(); i++)
    {
        const CTxOut& out = tx.vout[i];
        if (DecodeNameScript(out.scriptPubKey, nti, checkValuesCorrectness, checkAddressAndIfIsMine))
        {
            // If more than one name op, fail
            if (found)
                return false;

            nti.nOut = i;
            found = true;
        }
    }

    return found;
}

bool GetTxFee(const CTransaction& tx, bool fBlock, bool fMiner, int64& txFee)
{
    CTxDB txdb("r");
    MapPrevTx mapInputs;
    map<uint256, CTxIndex> mapUnused;
    bool fInvalid = false;
    if (!tx.FetchInputs(txdb, mapUnused, fBlock, fMiner, mapInputs, fInvalid))
        return false;
    txFee = tx.GetValueIn(mapInputs) - tx.GetValueOut();
    return true;
}

bool GetValueOfNameTx(const CTransaction& tx, vector<unsigned char>& value)
{
    NameTxInfo nti;
    if (!DecodeNameTx(tx, nti))
        return false;

    switch (nti.op)
    {
        case OP_NAME_NEW:
            value = nti.vchValue;
            return true;
        case OP_NAME_UPDATE:
            value = nti.vchValue;
            return true;
        default:
            return false;
    }
}

int IndexOfNameOutput(const CTransaction& tx)
{
    NameTxInfo nti;
    bool good = DecodeNameTx(tx, nti);

    if (!good)
        throw runtime_error("IndexOfNameOutput() : name output not found");
    return nti.nOut;
}

void CNamecoinHooks::AcceptToMemoryPool(CTxDB& txdb, const CTransaction& tx)
{
    if (tx.nVersion != NAMECOIN_TX_VERSION)
        return;

    if (tx.vout.size() < 1)
    {
        error("AcceptToMemoryPool() : no output in name tx %s\n", tx.ToString().c_str());
        return;
    }

    NameTxInfo nti;
    bool good = DecodeNameTx(tx, nti);

    if (!good)
    {
        error("AcceptToMemoryPool() : no output out script in name tx %s", tx.ToString().c_str());
        return;
    }

    {
        LOCK(cs_main);
        mapNamePending[nti.vchName].insert(tx.GetHash());
    }
}

// Returns true if tx is a valid namecoin tx.
// Will write to nameindex.dat if fBlock=true.
// Will remove incorrect namecoin tx or non-namecoin tx that tries to spend namecoin inputs from wallet and memory pool if fMiner=true. TODO: this should be done elsewhere (perhaps at mempool.accept)
bool CNamecoinHooks::ConnectInputs(CTxDB& txdb,
        map<uint256, CTxIndex>& mapTestPool,
        const CTransaction& tx,
        vector<CTransaction>& vTxPrev, //vector of all input transactions
        vector<CTxIndex>& vTxindex,
        const CBlockIndex* pindexBlock,
        const CDiskTxPos& txPos,
        bool fBlock,
        bool fMiner)
{
    // find prev name tx, if it exists
    int nInput = 0;
    bool found = false;
    NameTxInfo prev_nti;
    for (int i = 0; i < tx.vin.size(); i++) //this scans all scripts of tx.vin
    {
        CTxOut& out = vTxPrev[i].vout[tx.vin[i].prevout.n];

        if (DecodeNameScript(out.scriptPubKey, prev_nti))
        {
            if (found)
                return error("ConnectInputHook() : multiple previous name transactions");
            found = true;
            nInput = i;
        }
    }

    if (tx.nVersion != NAMECOIN_TX_VERSION)
    {
        // Make sure name-op outputs are not spent by a regular transaction, or the name
        // would be lost
        if (found)
        {
            if (fMiner)
            {
                //TODO: move this elsewhere. Idealy this tx should fail to get into memory pool in the first place.
                pwalletMain->EraseFromWallet(tx.GetHash());
                mempool.remove(tx);
            }
            return error("ConnectInputHook() : a non-namecoin transaction with a namecoin input");
        }
        return false;
    }

    NameTxInfo nti;
    if (!DecodeNameTx(tx, nti))
        return error("ConnectInputsHook() : could not decode a namecoin tx");

    vector<unsigned char> vchName = nti.vchName;
    vector<unsigned char> vchValue = nti.vchValue;
    int nRentalDays = nti.nRentalDays;
    int op = nti.op;

    {
        //removeme
//        CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
//        ssTx << tx;
//        string strHex = HexStr(ssTx.begin(), ssTx.end());

        if (GetBoolArg("-printNamecoinConnectInputs"))
            printf("name = %s, value = %s, fBlock = %d, fMiner = %d, hex = %s\n",
               stringFromVch(vchName).c_str(), stringFromVch(vchValue).c_str(), fBlock, fMiner, tx.GetHash().GetHex().c_str());
    }

    CNameDB dbName("cr+", txdb);

    switch (op)
    {
        case OP_NAME_NEW:
        {
            {
                //scan last 10 PoW block for tx fee that matches the one specified in tx
                const CBlockIndex* lastPoW = GetLastBlockIndex(pindexBlock, false);
                bool txFeePass = false;
                int64 txFee;
                if (!GetTxFee(tx, fBlock, fMiner, txFee) && fMiner)
                {
                    if (fMiner)
                    { //when generating new block remove namecoin tx with invalid inputs
                        //TODO: move this elsewhere. Idealy this tx should fail to get into memory pool in the first place.
                        pwalletMain->EraseFromWallet(tx.GetHash());
                        mempool.remove(tx);
                    }
                    return error("ConnectInputsHook() : could not read fee from database. Removing tx from mempool.");
                }

                for (int i = 1; i <= 10; i++)
                {
                    int64 netFee = GetNameNewFee(lastPoW, nRentalDays);
                    //printf("op == name_new, txFee = %"PRI64d", netFee = %"PRI64d", nRentalDays = %d\n", txFee, netFee, nRentalDays);
                    if (txFee >= netFee)
                    {
                        txFeePass = true;
                        break;
                    }
                    lastPoW = GetLastBlockIndex(lastPoW->pprev, false);
                }
                if (!txFeePass)
                    return error("ConnectInputsHook() : got tx %s with fee too low %d.", tx.GetHash().GetHex().c_str(), txFee);
            }

            if (dbName.ExistsName(vchName))
            {
                int nTotalLifeTime, nPrevHeight;
                if (!GetExpirationData(vchName, nTotalLifeTime, nPrevHeight))
                    return error("ConnectInputsHook() : failed to get expiration data");;

                if (pindexBlock->nHeight - nPrevHeight < nTotalLifeTime)
                    return error("ConnectInputsHook() : name_new on an unexpired name");
            }

            if (fMiner)
            {
                // Check that no other pending txs on this name are already in the block to be mined
                set<uint256>& setPending = mapNamePending[vchName];
                BOOST_FOREACH(const PAIRTYPE(uint256, CTxIndex)& s, mapTestPool)
                {
                    if (setPending.count(s.first))
                        return error("ConnectInputsHook() : will not mine %s because it clashes with %s", tx.GetHash().GetHex().c_str(), s.first.GetHex().c_str());
                }
            }
            break;
        }
        case OP_NAME_UPDATE:
        {
            {
                //scan last 10 PoW block for tx fee that matches the one specified in tx
                const CBlockIndex* lastPoW = GetLastBlockIndex(pindexBlock, false);
                bool txFeePass = false;
                int64 txFee;
                if (!GetTxFee(tx, fBlock, fMiner, txFee) && fMiner)
                {
                    if (fMiner)
                    { //when generating new block remove namecoin tx with invalid inputs
                        //TODO: move this elsewhere. Idealy this tx should fail to get into memory pool in the first place.
                        pwalletMain->EraseFromWallet(tx.GetHash());
                        mempool.remove(tx);
                    }
                    return error("ConnectInputsHook() : could not read fee from database. Removing tx from mempool.");
                }

                for (int i = 1; i <= 10; i++)
                {
                    int64 netFee = GetNameUpdateFee(lastPoW, nRentalDays);
                    //printf("op == update, txFee = %"PRI64d", netFee = %"PRI64d", nRentalDays = %d\n", txFee, netFee, nRentalDays);
                    if (txFee >= netFee)
                    {
                        txFeePass = true;
                        break;
                    }
                    lastPoW = GetLastBlockIndex(lastPoW->pprev, false);
                }
                if (!txFeePass)
                    return error("ConnectInputsHook() : got tx %s with fee too low %d", tx.GetHash().GetHex().c_str(), txFee);
            }


            if (!found || (prev_nti.op != OP_NAME_NEW && prev_nti.op != OP_NAME_UPDATE))
                return error("name_update tx without previous new or update tx");

            // Check name
            if (prev_nti.vchName != vchName)
                return error("ConnectInputsHook() : name_update name mismatch");

            //check if name has expired
            int nTotalLifeTime, nPrevHeight;
            if (!GetExpirationData(vchName, nTotalLifeTime, nPrevHeight))
                return error("ConnectInputsHook() : failed to get expiration data");;

            if (pindexBlock->nHeight - nPrevHeight >= nTotalLifeTime)
                return error("ConnectInputsHook() : name_update on expired name");
            break;
        }
        default:
            return error("ConnectInputsHook() : name transaction has unknown op");
    }

    {
        //most checks have now passed - try to write it to NameDB
        vector<CNameIndex> vtxPos;
        if (dbName.ExistsName(vchName))
        {
            if (!dbName.ReadName(vchName, vtxPos))
                return error("ConnectInputsHook() : failed to read from name DB");
        }

        if (op == OP_NAME_UPDATE)
        {
            if (!CheckNameTxPos(vtxPos, vTxindex[nInput].pos))
                return error("ConnectInputsHook() : tx %s rejected, since previous tx (%s) is not in the name DB\n", tx.GetHash().ToString().c_str(), vTxPrev[nInput].GetHash().ToString().c_str());
        }

        if (fBlock)
        {
            dbName.TxnBegin();
            if (op == OP_NAME_NEW)
            {//try to delete previous chain of new->update->update->... from nameindex.dat
                if (!dbName.EraseName(vchName))
                    return error("ConnectInputsHook() : failed to write to name DB");
                vtxPos.clear();
            }
            if (op == OP_NAME_NEW || op == OP_NAME_UPDATE)
            {
                vector<unsigned char> vchValue; // add
                int nHeight;
                uint256 hash;
                GetValueOfTxPos(txPos, vchValue, hash, nHeight);
                CNameIndex txPos2;
                txPos2.nHeight = pindexBlock->nHeight;
                txPos2.vValue = vchValue;
                txPos2.txPos = txPos;
                vtxPos.push_back(txPos2); // fin add
                if (!dbName.WriteName(vchName, vtxPos))
                    return error("ConnectInputsHook() : failed to write to name DB");
                printf("connectInputs(): writing %s to nameindex.dat\n", stringFromVch(vchName).c_str());
            }

            {
                LOCK(cs_main);
                std::map<std::vector<unsigned char>, std::set<uint256> >::iterator mi = mapNamePending.find(vchName);
                if (mi != mapNamePending.end())
                    mi->second.erase(tx.GetHash());
            }
            if (!dbName.TxnCommit())
                return error("failed to write %s to name DB", stringFromVch(vchName).c_str());
        }
    }
    return true;
}

bool CNamecoinHooks::DisconnectInputs(CTxDB& txdb,
        const CTransaction& tx,
        CBlockIndex* pindexBlock)
{
    if (tx.nVersion != NAMECOIN_TX_VERSION)
        return true;

    NameTxInfo nti;
    bool good = DecodeNameTx(tx, nti);
    if (!good)
        return error("DisconnectInputsHook() : could not decode namecoin tx");
    if (nti.op == OP_NAME_NEW || nti.op == OP_NAME_UPDATE)
    {
        CNameDB dbName("cr+", txdb);

        dbName.TxnBegin();

        //vector<CDiskTxPos> vtxPos;
        vector<CNameIndex> vtxPos;
        if (!dbName.ReadName(nti.vchName, vtxPos))
            return error("DisconnectInputsHook() : failed to read from name DB");
        // vtxPos might be empty if we pruned expired transactions.  However, it should normally still not
        // be empty, since a reorg cannot go that far back.  Be safe anyway and do not try to pop if empty.
        if (vtxPos.size())
        {
            CTxIndex txindex;
            if (!txdb.ReadTxIndex(tx.GetHash(), txindex))
                return error("DisconnectInputsHook() : failed to read tx index");

            if (vtxPos.back().txPos == txindex.pos)
                vtxPos.pop_back();

            // TODO validate that the first pos is the current tx pos
        }
        if (!dbName.WriteName(nti.vchName, vtxPos))
            return error("DisconnectInputsHook() : failed to write to name DB");

        dbName.TxnCommit();
    }

    return true;
}

bool CNamecoinHooks::CheckTransaction(const CTransaction& tx)
{
    if (tx.nVersion != NAMECOIN_TX_VERSION)
        return true;

    NameTxInfo nti;
    bool good = DecodeNameTx(tx, nti);

    if (!good)
        return error("name transaction has unknown script format");
    return true;
}

static string nameFromOp(int op)
{
    switch (op)
    {
        case OP_NAME_UPDATE:
            return "name_update";
        case OP_NAME_NEW:
            return "name_new";
        default:
            return "<unknown name op>";
    }
}

bool CNamecoinHooks::ExtractAddress(const CScript& script, string& address)
{
    NameTxInfo nti;
    if (!DecodeNameScript(script, nti))
        return false;

    string strOp = nameFromOp(nti.op);
    address = strOp + ": " + stringFromVch(nti.vchName);
    return true;
}

bool CNamecoinHooks::ConnectBlock(CBlock& block, CTxDB& txdb, CBlockIndex* pindex)
{
    return true;
}

bool CNamecoinHooks::DisconnectBlock(CBlock& block, CTxDB& txdb, CBlockIndex* pindex)
{
    return true;
}

// if target is not a namecoin transaction, then we should not allow it to spend namecoin inputs
// returns true if coin can be spend
bool CNamecoinHooks::SelectCoinsMinConf(const CWalletTx* pcoin, int nVersion)
{
    if (nVersion != NAMECOIN_TX_VERSION && pcoin->nVersion == NAMECOIN_TX_VERSION)
        return false;
    else
        return true;
}

bool CNamecoinHooks::listunspent(int nVersion)
{
    return nVersion == NAMECOIN_TX_VERSION;
}



#include <boost/assign/list_of.hpp>
using namespace boost::assign;

void sha256(const uint256& input, uint256& output)
{
    SHA256((unsigned char*)&input, sizeof(input), (unsigned char*)&output);
}

string stringFromCKeyingMaterial(const CKeyingMaterial &vch) {
    string res;
    CKeyingMaterial::const_iterator vi = vch.begin();
    while (vi != vch.end()) {
        res += (char)(*vi);
        vi++;
    }
    return res;
}

Value name_encrypt(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 2 || params.size() > 3)
        throw runtime_error(
            "name_encrypt <msg> <sign> [to]\n"
            "Encrypts a string and returns a hex string. Does not alter wallet or blockchain.\n"
            "  msg: message to be signed\n"
            "  sign: key from key->value pair that belongs to you\n"
            "  to: [username1, username2..usernameN]\n");

    RPCTypeCheck(params, list_of(str_type)(str_type)(array_type), true);

    vector<unsigned char> msg = vchFromValue(params[0]);
    if (msg.size() == 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "message is too short");

    uint256 hash;
    SHA256(&msg[0], msg.size(), (unsigned char*)&hash);

    CCrypter crypter;

//set pass
    SecureString value;
    value = "value1";
    string str = "aaaabbbb"; //needs to be 8 byte size
    vector<unsigned char> salt(str.begin(), str.end());
    bool res1 = crypter.SetKeyFromPassphrase(value, salt, 25000, 0);

//encrypt
    str = "value1 value2 value3 value4";
    CKeyingMaterial message(str.begin(), str.end());
    vector<unsigned char> cipher;
    bool res2 = crypter.Encrypt(message, cipher);

    //printf("cipher text = %s", stringFromVch(cipher).c_str());

//decrypt
    CKeyingMaterial decryptedText;
    crypter.Decrypt(cipher, decryptedText);


    //printf("decrypted text = %s", stringFromCKeyingMaterial(decryptedText).c_str());




    Object result;
    result.push_back(Pair("msg", stringFromVch(msg)));
    result.push_back(Pair("hash", HexStr(BEGIN(hash), END(hash))));
    //result.push_back(Pair("crypter cipher hex", HexStr(cipher)));
    string t1 = EncodeBase58(cipher);
    vector<unsigned char> decoded;; DecodeBase58(t1,decoded);
    result.push_back(Pair("crypter cipher base58", t1));
    result.push_back(Pair("decoded == cipher", decoded == cipher));
    result.push_back(Pair("crypter pass", res1));
    result.push_back(Pair("crypter encrypt", res2));
    return result;
}


Value name_decrypt(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 2 || params.size() > 2)
        throw runtime_error(
            "name_decrypt <hexstring> [msg]\n"
            "Decrypts a hex string and returns string. Does not alter wallet or blockchain.\n"
            "hexstring: encrypted message\n"
            "to: [username1, username2..usernameN]\n"
            "sign: key from key->value pair that belongs to you.\n");

    Object result;
    return result;
}
