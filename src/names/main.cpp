// Copyright (c) 2014-2019 Daniel Kraft
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <names/main.h>

#include <chainparams.h>
#include <coins.h>
#include <consensus/validation.h>
#include <dbwrapper.h>
#include <hash.h>
#include <names/encoding.h>
#include <script/interpreter.h>
#include <script/names.h>
#include <script/script.h>
#include <txmempool.h>
#include <uint256.h>
#include <undo.h>
#include <util/system.h>
#include <util/strencodings.h>
#include <util/validation.h>
#include <validation.h>

#include <univalue.h>

#include <string>

namespace
{

constexpr unsigned MAX_VALUE_LENGTH = 2048;
constexpr unsigned MAX_NAME_LENGTH = 256;

/* Ensure that the name length fits to the script element size limit to avoid
   a situation as in Namecoin where names can become unspendable.  */
static_assert (MAX_VALUE_LENGTH <= MAX_SCRIPT_ELEMENT_SIZE,
               "Maximum value size is too large for script element size");
static_assert (MAX_NAME_LENGTH <= MAX_SCRIPT_ELEMENT_SIZE,
               "Maximum name size is too large for script element size");

}  // anonymous namespace

/* ************************************************************************** */
/* CNameTxUndo.  */

void
CNameTxUndo::fromOldState (const valtype& nm, const CCoinsView& view)
{
  name = nm;
  isNew = !view.GetName (name, oldData);
}

void
CNameTxUndo::apply (CCoinsViewCache& view) const
{
  if (isNew)
    view.DeleteName (name);
  else
    view.SetName (name, oldData, true);
}

/* ************************************************************************** */

bool
IsNameValid (const valtype& name, CValidationState& state)
{
  if (name.size () > MAX_NAME_LENGTH)
    return state.Invalid (ValidationInvalidReason::CONSENSUS, false,
                          REJECT_INVALID, "tx-name-too-long",
                          "The name is too long");

  /* All names must have a namespace.  This means that they must start with
     some lower-case letters and /.  As a regexp, that is: [a-z]+\/.* */
  bool foundNamespace = false;
  for (size_t i = 0; i < name.size (); ++i)
    {
      if (name[i] == '/')
        {
          if (i == 0)
            return state.Invalid (ValidationInvalidReason::CONSENSUS, false,
                                  REJECT_INVALID, "tx-name-empty-namespace",
                                  "The empty namespace is not valid");

          foundNamespace = true;
          break;
        }

      if (name[i] < 'a' || name[i] > 'z')
        return state.Invalid (ValidationInvalidReason::CONSENSUS, false,
                              REJECT_INVALID, "tx-name-invalid-namespace",
                              "The namespace must only consist of lower-case"
                              " letters");
    }
  if (!foundNamespace)
    return state.Invalid (ValidationInvalidReason::CONSENSUS, false,
                          REJECT_INVALID, "tx-name-no-namespace",
                          "The name has no namespace");

  /* Non-printable ASCII characters are not allowed.  This check works also for
     UTF-8 encoded strings, as characters <0x80 are encoded as a single byte
     and never occur as part of some other UTF-8 sequence.  */
  for (const unsigned char c : name)
    if (c < 0x20)
      return state.Invalid (ValidationInvalidReason::CONSENSUS, false,
                            REJECT_INVALID, "tx-name-unprintable-ascii",
                            "Non-printable ASCII characters are not allowed"
                            " in names");

  /* Only valid UTF-8 strings can be names.  */
  if (!IsValidUtf8String (std::string (name.begin (), name.end ())))
    return state.Invalid (ValidationInvalidReason::CONSENSUS, false,
                          REJECT_INVALID, "tx-name-invalid-utf8",
                          "The name is not valid UTF-8");

  return true;
}

bool
IsValueValid (const valtype& value, CValidationState& state)
{
  if (value.size () > MAX_VALUE_LENGTH)
    return state.Invalid (ValidationInvalidReason::CONSENSUS, false,
                          REJECT_INVALID, "tx-value-too-long",
                          "The value is too long");

  /* The value must parse with Univalue as JSON and be an object.  */
  UniValue jsonValue;
  if (!jsonValue.read (std::string (value.begin (), value.end ())))
    return state.Invalid (ValidationInvalidReason::CONSENSUS, false,
                          REJECT_INVALID, "tx-value-invalid-json",
                          "The value is not valid JSON");
  if (!jsonValue.isObject ())
    return state.Invalid (ValidationInvalidReason::CONSENSUS, false,
                          REJECT_INVALID, "tx-value-no-json-object",
                          "The value must be a JSON object");

  return true;
}

bool
CheckNameTransaction (const CTransaction& tx, unsigned nHeight,
                      const CCoinsView& view,
                      CValidationState& state)
{
  /* As a first step, try to locate inputs and outputs of the transaction
     that are name scripts.  At most one input and output should be
     a name operation.  */

  int nameIn = -1;
  CNameScript nameOpIn;
  Coin coinIn;
  for (unsigned i = 0; i < tx.vin.size (); ++i)
    {
      const COutPoint& prevout = tx.vin[i].prevout;
      Coin coin;
      if (!view.GetCoin (prevout, coin))
        return state.Invalid (ValidationInvalidReason::TX_MISSING_INPUTS, false,
                              REJECT_INVALID, "bad-txns-inputs-missingorspent",
                              "Failed to fetch name input coin");

      const CNameScript op(coin.out.scriptPubKey);
      if (op.isNameOp ())
        {
          if (nameIn != -1)
            return state.Invalid (ValidationInvalidReason::CONSENSUS, false,
                                  REJECT_INVALID, "tx-multiple-name-inputs",
                                  "Multiple name inputs");
          nameIn = i;
          nameOpIn = op;
          coinIn = coin;
        }
    }

  int nameOut = -1;
  CNameScript nameOpOut;
  for (unsigned i = 0; i < tx.vout.size (); ++i)
    {
      const CNameScript op(tx.vout[i].scriptPubKey);
      if (op.isNameOp ())
        {
          if (nameOut != -1)
            return state.Invalid (ValidationInvalidReason::CONSENSUS, false,
                                  REJECT_INVALID, "tx-multiple-name-outputs",
                                  "Multiple name outputs");
          nameOut = i;
          nameOpOut = op;
        }
    }

  /* If there are no name outputs, then this transaction is not a name
     operation.  In this case, there should also be no name inputs, but
     otherwise the validation is done.  */
  if (nameOut == -1)
    {
      if (nameIn != -1)
        return state.Invalid (ValidationInvalidReason::CONSENSUS, false,
                              REJECT_INVALID, " tx-name-in-no-name-out",
                              "Transaction has name input but no name output");
      return true;
    }

  /* Reject "greedy names".  */
  const Consensus::Params& params = Params ().GetConsensus ();
  if (tx.vout[nameOut].nValue < params.rules->MinNameCoinAmount(nHeight))
    return state.Invalid (ValidationInvalidReason::CONSENSUS, false,
                          REJECT_INVALID, "tx-name-greedy",
                          "Greedy name operation");

  /* Now that we have ruled out NAME_NEW, check that we have a previous
     name input that is being updated.  */

  assert (nameOpOut.isAnyUpdate ());
  if (nameOpOut.getNameOp () == OP_NAME_REGISTER) {
    if (nameIn != -1)
      return state.Invalid (ValidationInvalidReason::CONSENSUS, false,
                            REJECT_INVALID, "tx-nameregister-without-name-in",
                            "NAME_REGISTER without name input");
  }
  else if (nameIn == -1)
    return state.Invalid (ValidationInvalidReason::CONSENSUS, false,
                          REJECT_INVALID, "tx-nameupdate-without-name-input",
                          "Name update has no previous name input");
  const valtype& name = nameOpOut.getOpName ();

  if (!IsNameValid (name, state))
    {
      error ("%s: Name is invalid: %s", __func__, FormatStateMessage (state));
      return false;
    }
  if (!IsValueValid (nameOpOut.getOpValue (), state))
    {
      error ("%s: Value is invalid: %s", __func__, FormatStateMessage (state));
      return false;
    }

  /* Process NAME_UPDATE next.  */

  if (nameOpOut.getNameOp () == OP_NAME_UPDATE)
    {
      if (!nameOpIn.isAnyUpdate ())
        return state.Invalid (ValidationInvalidReason::CONSENSUS, false,
                              REJECT_INVALID, "tx-nameupdate-invalid-prev",
                              "Name input for NAME_UPDATE is not an update");

      if (name != nameOpIn.getOpName ())
        return state.Invalid (ValidationInvalidReason::CONSENSUS, false,
                              REJECT_INVALID, "tx-nameupdate-name-mismatch",
                              "NAME_UPDATE name mismatch to name input");

      /* If the name input is pending, then no further checks with respect
         to the name input in the name database are done.  Otherwise, we verify
         that the name input matches the name database; this is redundant
         as UTXO handling takes care of it anyway, but we do it for
         an extra safety layer.  */
      const unsigned inHeight = coinIn.nHeight;
      if (inHeight == MEMPOOL_HEIGHT)
        return true;

      CNameData oldName;
      if (!view.GetName (name, oldName))
        return state.Invalid (ValidationInvalidReason::CONSENSUS, false,
                              REJECT_INVALID, "tx-nameupdate-nonexistant",
                              "NAME_UPDATE name does not exist");
      assert (inHeight == oldName.getHeight ());
      assert (tx.vin[nameIn].prevout == oldName.getUpdateOutpoint ());

      return true;
    }

  /* Finally, NAME_REGISTER.  */

  CNameData oldName;
  if (view.GetName (name, oldName))
    return state.Invalid (ValidationInvalidReason::CONSENSUS, false,
                          REJECT_INVALID, "tx-nameregister-existing-name",
                          "NAME_REGISTER on existing name");

  /* We don't have to specifically check that miners don't create blocks with
     conflicting NAME_FIRSTUPDATE's, since the mining's CCoinsViewCache
     takes care of this with the check above already.  */

  return true;
}

void
ApplyNameTransaction (const CTransaction& tx, unsigned nHeight,
                      CCoinsViewCache& view, CBlockUndo& undo)
{
  assert (nHeight != MEMPOOL_HEIGHT);

  /* Changes are encoded in the outputs.  We don't have to do any checks,
     so simply apply all these.  */

  for (unsigned i = 0; i < tx.vout.size (); ++i)
    {
      const CNameScript op(tx.vout[i].scriptPubKey);
      if (op.isNameOp () && op.isAnyUpdate ())
        {
          const valtype& name = op.getOpName ();
          LogPrint (BCLog::NAMES, "Updating name at height %d: %s\n",
                    nHeight, EncodeNameForMessage (name));

          CNameTxUndo opUndo;
          opUndo.fromOldState (name, view);
          undo.vnameundo.push_back (opUndo);

          CNameData data;
          data.fromScript (nHeight, COutPoint (tx.GetHash (), i), op);
          view.SetName (name, data, false);
        }
    }
}

void
CheckNameDB (bool disconnect)
{
  const int option
    = gArgs.GetArg ("-checknamedb", Params ().DefaultCheckNameDB ());

  if (option == -1)
    return;

  assert (option >= 0);
  if (option != 0)
    {
      if (disconnect || ::ChainActive ().Height () % option != 0)
        return;
    }

  auto& coinsTip = ::ChainstateActive ().CoinsTip ();
  coinsTip.Flush ();
  assert (coinsTip.ValidateNameDB ());
}
