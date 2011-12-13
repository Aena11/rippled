#include "LedgerHistory.h"
#include "Config.h"
#include "Application.h"
#include <string>

#if 0
bool LedgerHistory::loadLedger(const uint256& hash)
{
	Ledger::pointer ledger=Ledger::pointer(new Ledger());
	if(ledger->load(hash))
	{
		mAllLedgers[hash]=ledger;
		return(true);
	}
	return(false);
}

bool LedgerHistory::loadAcceptedLedger(uint32 index)
{
	// TODO: LedgerHistory::loadAcceptedLedger(uint32 index)
	/*
	Ledger::pointer ledger=theApp->getSerializer()->loadAcceptedLedger(index);
	if(ledger)
	{
		mAcceptedLedgers[index]=ledger;
		return(true);
	}*/
	return(false);
}

void LedgerHistory::addAcceptedLedger(Ledger::pointer ledger)
{
	mAcceptedLedgers[ledger->getIndex()]=ledger;
}

// this will see if the ledger is in memory
// if not it will check disk and load it
// if not it will return NULL
Ledger::pointer LedgerHistory::getAcceptedLedger(uint32 index)
{
	if(mAcceptedLedgers.count(index))
		return(mAcceptedLedgers[index]);
	if(loadAcceptedLedger(index)) return(mAcceptedLedgers[index]);
	return(Ledger::pointer());
}

void LedgerHistory::addLedger(Ledger::pointer ledger)
{
	mAcceptedLedgers[ledger->getIndex()]=ledger;
	ledger->save();
}

Ledger::pointer LedgerHistory::getLedger(const uint256& hash)
{
	if(mAllLedgers.count(hash))
		return(mAllLedgers[hash]);
	if(loadLedger(hash)) return(mAllLedgers[hash]);
	return(Ledger::pointer());
}
#endif
