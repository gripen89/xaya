// Copyright (c) 2018-2019 Daniel Kraft
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_RPC_AUXPOW_MINER_H
#define BITCOIN_RPC_AUXPOW_MINER_H

#include <miner.h>
#include <powdata.h>
#include <script/script.h>
#include <script/standard.h>
#include <sync.h>
#include <uint256.h>
#include <univalue.h>

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace auxpow_tests
{
class AuxpowMinerForTest;
}

/**
 * This class holds "global" state used to construct blocks for the auxpow
 * mining RPCs and the map of already constructed blocks to look them up
 * in the submitauxblock RPC.
 *
 * It is used as a singleton that is initialised during startup, taking the
 * place of the previously real global and static variables.
 */
class AuxpowMiner
{

private:

  /** The lock used for state in this object.  */
  mutable CCriticalSection cs;
  /** All currently "active" block templates.  */
  std::vector<std::unique_ptr<CBlockTemplate>> templates;
  /** Maps block hashes to pointers in vTemplates.  Does not own the memory.  */
  std::map<uint256, const CBlock*> blocks;
  /**
   * Maps coinbase script hashes and PoW algorithms to pointers in vTemplates.
   * Does not own the memory.
   */
  std::map<std::pair<PowAlgo, CScriptID>, const CBlock*> curBlocks;

  /** The current extra nonce for block creation.  */
  unsigned extraNonce = 0;

  /* Some data about when the current block (pblock) was constructed.  */
  unsigned txUpdatedLast;
  const CBlockIndex* pindexPrev = nullptr;
  uint64_t startTime;

  /**
   * Constructs a new current block if necessary (checking the current state to
   * see if "enough changed" for this), and returns a pointer to the block
   * that should be returned to a miner for working on at the moment.  Also
   * fills in the difficulty target value.
   */
  const CBlock* getCurrentBlock (PowAlgo algo, const CScript& scriptPubKey,
                                 uint256& target);

  /**
   * Looks up a previously constructed block by its (hex-encoded) hash.  If the
   * block is found, it is returned.  Otherwise, a JSONRPCError is thrown.
   */
  const CBlock* lookupSavedBlock (const std::string& hashHex) const;

  friend class auxpow_tests::AuxpowMinerForTest;

public:

  AuxpowMiner () = default;

  /**
   * Performs the main work for the "createauxblock" RPC:  Construct a new block
   * to work on with the given address for the block reward and return the
   * necessary information for the miner to construct an auxpow for it.
   */
  UniValue createAuxBlock (const CScript& scriptPubKey);

  /**
   * Performs the main work for the "submitauxblock" RPC:  Look up the block
   * previously created for the given hash, attach the given auxpow to it
   * and try to submit it.  Returns true if all was successful and the block
   * was accepted.
   */
  bool submitAuxBlock (const std::string& hashHex,
                       const std::string& auxpowHex) const;

  /**
   * Performs the main logic needed for the "create" form of the "getwork" RPC.
   */
  UniValue createWork (const CScript& scriptPubKey);

  /**
   * Performs the "submit" form of the "getwork" RPC.
   */
  bool submitWork (const std::string& hashHex,
                   const std::string& dataHex) const;

  /**
   * Returns the singleton instance of AuxpowMiner that is used for RPCs.
   */
  static AuxpowMiner& get ();

};

#endif // BITCOIN_RPC_AUXPOW_MINER_H
