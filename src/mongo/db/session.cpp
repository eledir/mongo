/**
 *    Copyright (C) 2017 MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/session.h"

#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/transaction_history_iterator.h"
#include "mongo/stdx/memory.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace {

void updateSessionEntry(OperationContext* opCtx, const UpdateRequest& updateRequest) {
    AutoGetCollection autoColl(opCtx, NamespaceString::kSessionTransactionsTableNamespace, MODE_IX);

    uassert(40527,
            str::stream() << "Unable to persist transaction state because the session transaction "
                             "collection is missing. This indicates that the "
                          << NamespaceString::kSessionTransactionsTableNamespace.ns()
                          << " collection has been manually deleted.",
            autoColl.getCollection());

    const auto updateResult = update(opCtx, autoColl.getDb(), updateRequest);

    if (!updateResult.numDocsModified && updateResult.upserted.isEmpty()) {
        throw WriteConflictException();
    }
}

// Failpoint which allows different failure actions to happen after each write. Supports the
// parameters below, which can be combined with each other (unless explicitly disallowed):
//
// closeConnection (bool, default = true): Closes the connection on which the write was executed.
// failBeforeCommitExceptionCode (int, default = not specified): If set, the specified exception
//      code will be thrown, which will cause the write to not commit; if not specified, the write
//      will be allowed to commit.
MONGO_FP_DECLARE(onPrimaryTransactionalWrite);

}  // namespace

Session::Session(LogicalSessionId sessionId) : _sessionId(std::move(sessionId)) {}

void Session::refreshFromStorageIfNeeded(OperationContext* opCtx) {
    invariant(!opCtx->lockState()->isLocked());
    invariant(repl::ReadConcernArgs::get(opCtx).getLevel() ==
              repl::ReadConcernLevel::kLocalReadConcern);

    stdx::unique_lock<stdx::mutex> ul(_mutex);

    while (!_isValid) {
        const int numInvalidations = _numInvalidations;

        ul.unlock();

        const auto lastWrittenTxnRecord = [&]() -> boost::optional<SessionTxnRecord> {
            DBDirectClient client(opCtx);
            auto result = client.findOne(
                NamespaceString::kSessionTransactionsTableNamespace.ns(),
                {BSON(SessionTxnRecord::kSessionIdFieldName << _sessionId.toBSON())});
            if (result.isEmpty()) {
                return boost::none;
            }

            return SessionTxnRecord::parse(
                IDLParserErrorContext("parse latest txn record for session"), result);
        }();

        ul.lock();

        // Protect against concurrent refreshes or invalidations
        if (!_isValid && _numInvalidations == numInvalidations) {
            _isValid = true;
            _lastWrittenSessionRecord = std::move(lastWrittenTxnRecord);

            if (_lastWrittenSessionRecord) {
                _activeTxnNumber = _lastWrittenSessionRecord->getTxnNum();
            }

            break;
        }
    }
}

void Session::beginTxn(OperationContext* opCtx, TxnNumber txnNumber) {
    invariant(!opCtx->lockState()->isLocked());

    stdx::lock_guard<stdx::mutex> lg(_mutex);
    _beginTxn(lg, txnNumber);
}

void Session::onWriteOpCompletedOnPrimary(OperationContext* opCtx,
                                          TxnNumber txnNumber,
                                          std::vector<StmtId> stmtIdsWritten,
                                          Timestamp newLastWriteTs) {
    invariant(opCtx->lockState()->inAWriteUnitOfWork());

    stdx::unique_lock<stdx::mutex> ul(_mutex);
    _checkValid(ul);
    _checkIsActiveTransaction(ul, txnNumber);

    const auto updateRequest = _makeUpdateRequest(ul, txnNumber, newLastWriteTs);

    ul.unlock();

    repl::UnreplicatedWritesBlock doNotReplicateWrites(opCtx);

    updateSessionEntry(opCtx, updateRequest);
    _registerUpdateCacheOnCommit(opCtx, txnNumber, std::move(stmtIdsWritten), newLastWriteTs);
}

void Session::updateSessionRecordOnSecondary(OperationContext* opCtx,
                                             const SessionTxnRecord& sessionTxnRecord) {
    invariant(!opCtx->lockState()->isLocked());

    writeConflictRetry(
        opCtx, "Update session txn", NamespaceString::kSessionTransactionsTableNamespace.ns(), [&] {
            UpdateRequest updateRequest(NamespaceString::kSessionTransactionsTableNamespace);
            updateRequest.setUpsert(true);
            updateRequest.setQuery(BSON(SessionTxnRecord::kSessionIdFieldName
                                        << sessionTxnRecord.getSessionId().toBSON()));
            updateRequest.setUpdates(sessionTxnRecord.toBSON());

            repl::UnreplicatedWritesBlock doNotReplicateWrites(opCtx);

            Lock::DBLock configDBLock(opCtx, NamespaceString::kConfigDb, MODE_IX);
            WriteUnitOfWork wuow(opCtx);
            updateSessionEntry(opCtx, updateRequest);
            wuow.commit();
        });
}

void Session::invalidate() {
    stdx::lock_guard<stdx::mutex> lg(_mutex);
    _isValid = false;
    _numInvalidations++;

    _lastWrittenSessionRecord.reset();

    _activeTxnNumber = kUninitializedTxnNumber;
}

Timestamp Session::getLastWriteOpTimeTs(TxnNumber txnNumber) const {
    stdx::lock_guard<stdx::mutex> lg(_mutex);
    _checkValid(lg);
    _checkIsActiveTransaction(lg, txnNumber);

    if (!_lastWrittenSessionRecord || _lastWrittenSessionRecord->getTxnNum() != txnNumber)
        return Timestamp();

    return _lastWrittenSessionRecord->getLastWriteOpTimeTs();
}

boost::optional<repl::OplogEntry> Session::checkStatementExecuted(OperationContext* opCtx,
                                                                  TxnNumber txnNumber,
                                                                  StmtId stmtId) const {
    stdx::unique_lock<stdx::mutex> ul(_mutex);
    _checkValid(ul);
    _checkIsActiveTransaction(ul, txnNumber);

    if (!_lastWrittenSessionRecord || _lastWrittenSessionRecord->getTxnNum() != txnNumber)
        return boost::none;

    auto it = TransactionHistoryIterator(_lastWrittenSessionRecord->getLastWriteOpTimeTs());

    ul.unlock();

    while (it.hasNext()) {
        const auto entry = it.next(opCtx);
        invariant(entry.getStatementId());
        if (*entry.getStatementId() == stmtId) {
            return entry;
        }
    }

    return boost::none;
}

void Session::_beginTxn(WithLock wl, TxnNumber txnNumber) {
    _checkValid(wl);

    uassert(ErrorCodes::TransactionTooOld,
            str::stream() << "Cannot start transaction " << txnNumber << " on session "
                          << getSessionId()
                          << " because a newer transaction "
                          << _activeTxnNumber
                          << " has already started.",
            txnNumber >= _activeTxnNumber);

    // Check for continuing an existing transaction
    if (txnNumber == _activeTxnNumber)
        return;

    _activeTxnNumber = txnNumber;
}

void Session::_checkValid(WithLock) const {
    uassert(ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "Session " << getSessionId()
                          << " was concurrently modified and the operation must be retried.",
            _isValid);
}

void Session::_checkIsActiveTransaction(WithLock, TxnNumber txnNumber) const {
    uassert(ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "Cannot perform retryability check for transaction " << txnNumber
                          << " on session "
                          << getSessionId()
                          << " because a different transaction "
                          << _activeTxnNumber
                          << " is now active.",
            txnNumber == _activeTxnNumber);
}

UpdateRequest Session::_makeUpdateRequest(WithLock,
                                          TxnNumber newTxnNumber,
                                          Timestamp newLastWriteTs) const {
    UpdateRequest updateRequest(NamespaceString::kSessionTransactionsTableNamespace);
    updateRequest.setUpsert(true);

    if (_lastWrittenSessionRecord) {
        updateRequest.setQuery(_lastWrittenSessionRecord->toBSON());
        updateRequest.setUpdates(
            BSON("$set" << BSON(SessionTxnRecord::kTxnNumFieldName
                                << newTxnNumber
                                << SessionTxnRecord::kLastWriteOpTimeTsFieldName
                                << newLastWriteTs)));
    } else {
        const auto updateBSON = [&] {
            SessionTxnRecord newTxnRecord;
            newTxnRecord.setSessionId(_sessionId);
            newTxnRecord.setTxnNum(newTxnNumber);
            newTxnRecord.setLastWriteOpTimeTs(newLastWriteTs);
            return newTxnRecord.toBSON();
        }();

        updateRequest.setQuery(updateBSON);
        updateRequest.setUpdates(updateBSON);
    }

    return updateRequest;
}

void Session::_registerUpdateCacheOnCommit(OperationContext* opCtx,
                                           TxnNumber newTxnNumber,
                                           std::vector<StmtId> stmtIdsWritten,
                                           Timestamp newLastWriteTs) {
    opCtx->recoveryUnit()->onCommit(
        [ this, newTxnNumber, stmtIdsWritten = std::move(stmtIdsWritten), newLastWriteTs ] {
            stdx::lock_guard<stdx::mutex> lg(_mutex);

            if (!_isValid)
                return;

            if (newTxnNumber < _activeTxnNumber)
                return;

            // This call is necessary in order to advance the txn number and reset the cached state
            // in the case where just before the storage transaction commits, the cache entry gets
            // invalidated and immediately refreshed while there were no writes for newTxnNumber
            // yet. In this case _activeTxnNumber will be less than newTxnNumber and we will fail to
            // update the cache even though the write was successful.
            _beginTxn(lg, newTxnNumber);

            if (!_lastWrittenSessionRecord) {
                _lastWrittenSessionRecord.emplace();

                _lastWrittenSessionRecord->setSessionId(_sessionId);
                _lastWrittenSessionRecord->setTxnNum(newTxnNumber);
                _lastWrittenSessionRecord->setLastWriteOpTimeTs(newLastWriteTs);
            } else {
                if (newTxnNumber > _lastWrittenSessionRecord->getTxnNum())
                    _lastWrittenSessionRecord->setTxnNum(newTxnNumber);

                if (newLastWriteTs > _lastWrittenSessionRecord->getLastWriteOpTimeTs())
                    _lastWrittenSessionRecord->setLastWriteOpTimeTs(newLastWriteTs);
            }
        });

    MONGO_FAIL_POINT_BLOCK(onPrimaryTransactionalWrite, customArgs) {
        const auto& data = customArgs.getData();

        const auto closeConnectionElem = data["closeConnection"];
        if (closeConnectionElem.eoo() || closeConnectionElem.Bool()) {
            auto transportSession = opCtx->getClient()->session();
            transportSession->getTransportLayer()->end(transportSession);
        }

        const auto failBeforeCommitExceptionElem = data["failBeforeCommitExceptionCode"];
        if (!failBeforeCommitExceptionElem.eoo()) {
            const auto failureCode =
                ErrorCodes::fromInt(int(failBeforeCommitExceptionElem.Number()));
            uasserted(failureCode,
                      str::stream() << "Failing write for " << _sessionId << ":" << newTxnNumber
                                    << " due to failpoint. The write must not be reflected.");
        }
    }
}

}  // namespace mongo
