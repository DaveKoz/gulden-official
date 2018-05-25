// Copyright (c) 2011-2016 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// File contains modifications by: The Gulden developers
// All modifications:
// Copyright (c) 2016-2018 The Gulden developers
// Authored by: Malcolm MacLeod (mmacleod@webmail.co.za)
// Distributed under the GULDEN software license, see the accompanying
// file COPYING

#include "transactiontablemodel.h"

#include "addresstablemodel.h"
#include "guiconstants.h"
#include "gui.h" //For uuid variant.
#include "guiutil.h"
#include "optionsmodel.h"
#include "transactiondesc.h"
#include "transactionrecord.h"
#include "walletmodel.h"

#include "core_io.h"
#include "validation.h"
#include "sync.h"
#include "uint256.h"
#include "util.h"
#include "wallet/wallet.h"

#include <QColor>
#include <QDateTime>
#include <QDebug>
#include <QIcon>
#include <QList>

#include <Gulden/util.h>

// Amount column is right-aligned it contains numbers
static int column_alignments[] = {
        Qt::AlignLeft|Qt::AlignVCenter, /* status */
        Qt::AlignLeft|Qt::AlignVCenter, /* watchonly */
        Qt::AlignLeft|Qt::AlignVCenter, /* date */
        Qt::AlignLeft|Qt::AlignVCenter, /* type */
        Qt::AlignLeft|Qt::AlignVCenter, /* address */
        Qt::AlignRight|Qt::AlignVCenter, /* amount received */
        Qt::AlignRight|Qt::AlignVCenter /* amount sent */
    };

// Comparison operator for sort/binary search of model tx list
struct TxLessThan
{
    bool operator()(const TransactionRecord &a, const TransactionRecord &b) const
    {
        return a.hash < b.hash;
    }
    bool operator()(const TransactionRecord &a, const uint256 &b) const
    {
        return a.hash < b;
    }
    bool operator()(const uint256 &a, const TransactionRecord &b) const
    {
        return a < b.hash;
    }
};

// Private implementation
class TransactionTablePriv
{
public:
    TransactionTablePriv(CWallet *_wallet, TransactionTableModel *_parent) :
        wallet(_wallet),
        parent(_parent)
    {
    }

    CWallet *wallet;
    TransactionTableModel *parent;

    /* Local cache of wallet.
     * As it is in the same order as the CWallet, by definition
     * this is sorted by sha256.
     */
    QList<TransactionRecord> cachedWallet;

    /* Query entire wallet anew from core.
     */
    void refreshWallet()
    {
        qDebug() << "TransactionTablePriv::refreshWallet";
        cachedWallet.clear();
        {
            LOCK2(cs_main, wallet->cs_wallet);
            for(std::map<uint256, CWalletTx>::iterator it = wallet->mapWallet.begin(); it != wallet->mapWallet.end(); ++it)
            {
                if(TransactionRecord::showTransaction(it->second))
                    cachedWallet.append(TransactionRecord::decomposeTransaction(wallet, it->second));
            }
        }
    }

    /* Update our model of the wallet incrementally, to synchronize our model of the wallet
       with that of the core.

       Call with transaction that was added, removed or changed.
     */
    void updateWallet(const uint256 &hash, int status, bool showTransaction)
    {
        qDebug() << "TransactionTablePriv::updateWallet: " + QString::fromStdString(hash.ToString()) + " " + QString::number(status);

        // Find bounds of this transaction in model
        QList<TransactionRecord>::iterator lower = qLowerBound(
            cachedWallet.begin(), cachedWallet.end(), hash, TxLessThan());
        QList<TransactionRecord>::iterator upper = qUpperBound(
            cachedWallet.begin(), cachedWallet.end(), hash, TxLessThan());
        int lowerIndex = (lower - cachedWallet.begin());
        int upperIndex = (upper - cachedWallet.begin());
        bool inModel = (lower != upper);

        if(status == CT_UPDATED)
        {
            if(showTransaction && !inModel)
                status = CT_NEW; /* Not in model, but want to show, treat as new */
            if(!showTransaction && inModel)
                status = CT_DELETED; /* In model, but want to hide, treat as deleted */
        }

        qDebug() << "    inModel=" + QString::number(inModel) +
                    " Index=" + QString::number(lowerIndex) + "-" + QString::number(upperIndex) +
                    " showTransaction=" + QString::number(showTransaction) + " derivedStatus=" + QString::number(status);

        switch(status)
        {
        case CT_NEW:
            if(inModel)
            {
                qWarning() << "TransactionTablePriv::updateWallet: Warning: Got CT_NEW, but transaction is already in model";
                break;
            }
            if(showTransaction)
            {
                LOCK2(cs_main, wallet->cs_wallet);
                // Find transaction in wallet
                std::map<uint256, CWalletTx>::iterator mi = wallet->mapWallet.find(hash);
                if(mi == wallet->mapWallet.end())
                {
                    qWarning() << "TransactionTablePriv::updateWallet: Warning: Got CT_NEW, but transaction is not in wallet";
                    break;
                }
                // Added -- insert at the right position
                QList<TransactionRecord> toInsert =
                        TransactionRecord::decomposeTransaction(wallet, mi->second);
                if(!toInsert.isEmpty()) /* only if something to insert */
                {
                    parent->beginInsertRows(QModelIndex(), lowerIndex, lowerIndex+toInsert.size()-1);
                    int insert_idx = lowerIndex;
                    for(const TransactionRecord &rec : toInsert)
                    {
                        cachedWallet.insert(insert_idx, rec);
                        insert_idx += 1;
                    }
                    parent->endInsertRows();
                }
            }
            break;
        case CT_DELETED:
            if(!inModel)
            {
                qWarning() << "TransactionTablePriv::updateWallet: Warning: Got CT_DELETED, but transaction is not in model";
                break;
            }
            // Removed -- remove entire transaction from table
            parent->beginRemoveRows(QModelIndex(), lowerIndex, upperIndex-1);
            cachedWallet.erase(lower, upper);
            parent->endRemoveRows();
            break;
        case CT_UPDATED:
            // Miscellaneous updates -- nothing to do, status update will take care of this, and is only computed for
            // visible transactions.
            for (int i = lowerIndex; i < upperIndex; i++) {
                TransactionRecord *rec = &cachedWallet[i];
                rec->status.needsUpdate = true;
            }
            break;
        }
    }

    int size()
    {
        return cachedWallet.size();
    }

    TransactionRecord *index(int idx)
    {
        if(idx >= 0 && idx < cachedWallet.size())
        {
            TransactionRecord *rec = &cachedWallet[idx];

            // Get required locks upfront. This avoids the GUI from getting
            // stuck if the core is holding the locks for a longer time - for
            // example, during a wallet rescan.
            //
            // If a status update is needed (blocks came in since last check),
            //  update the status of this transaction from the wallet. Otherwise,
            // simply re-use the cached status.
            TRY_LOCK(cs_main, lockMain);
            if(lockMain)
            {
                TRY_LOCK(wallet->cs_wallet, lockWallet);
                if(lockWallet && rec->statusUpdateNeeded())
                {
                    std::map<uint256, CWalletTx>::iterator mi = wallet->mapWallet.find(rec->hash);

                    if(mi != wallet->mapWallet.end())
                    {
                        rec->updateStatus(mi->second);
                    }
                }
            }
            return rec;
        }
        return 0;
    }

    QString describe(TransactionRecord *rec, int unit)
    {
        {
            LOCK2(cs_main, wallet->cs_wallet);
            std::map<uint256, CWalletTx>::iterator mi = wallet->mapWallet.find(rec->hash);
            if(mi != wallet->mapWallet.end())
            {
                return TransactionDesc::toHTML(wallet, mi->second, rec, unit);
            }
        }
        return QString();
    }

    QString getTxHex(TransactionRecord *rec)
    {
        LOCK2(cs_main, wallet->cs_wallet);
        std::map<uint256, CWalletTx>::iterator mi = wallet->mapWallet.find(rec->hash);
        if(mi != wallet->mapWallet.end())
        {
            std::string strHex = EncodeHexTx(static_cast<CTransaction>(mi->second));
            return QString::fromStdString(strHex);
        }
        return QString();
    }

    int getTxBlockNumber(TransactionRecord *rec)
    {
        LOCK2(cs_main, wallet->cs_wallet);
        auto walletTxIter = wallet->mapWallet.find(rec->hash);
        if(walletTxIter != wallet->mapWallet.end())
        {
            auto blockIter = mapBlockIndex.find(walletTxIter->second.hashBlock);
            if (blockIter != mapBlockIndex.end())
            {
                return blockIter->second->nHeight;
            }
        }
        return 0;
    }
};

TransactionTableModel::TransactionTableModel(const QStyle *_platformStyle, CWallet* _wallet, WalletModel *parent):
        QAbstractTableModel(parent),
        wallet(_wallet),
        walletModel(parent),
        priv(new TransactionTablePriv(_wallet, this)),
        fProcessingQueuedTransactions(false),
        platformStyle(_platformStyle)
{
    columns << QString() << QString() << tr("Date") << tr("Type") << tr("Description") << tr("Received") << tr("Sent");
    priv->refreshWallet();

    connect(walletModel->getOptionsModel(), SIGNAL(displayUnitChanged(int)), this, SLOT(updateDisplayUnit()));

    subscribeToCoreSignals();
}

TransactionTableModel::~TransactionTableModel()
{
    unsubscribeFromCoreSignals();
    delete priv;
}

/** Updates the column title to "Amount (DisplayUnit)" and emits headerDataChanged() signal for table headers to react. */
void TransactionTableModel::updateAmountColumnTitle()
{
    /*columns[Amount] = GuldenUnits::getAmountColumnTitle(walletModel->getOptionsModel()->getDisplayUnit());
    Q_EMIT headerDataChanged(Qt::Horizontal,Amount,Amount);*/
}

void TransactionTableModel::updateTransaction(const QString &hash, int status, bool showTransaction)
{
    uint256 updated;
    updated.SetHex(hash.toStdString());

    priv->updateWallet(updated, status, showTransaction);
}

void TransactionTableModel::updateConfirmations()
{
    // Blocks came in since last poll.
    // Invalidate status (number of confirmations) and (possibly) description
    //  for all rows. Qt is smart enough to only actually request the data for the
    //  visible rows.
    Q_EMIT dataChanged(index(0, Status), index(priv->size()-1, Status));
    Q_EMIT dataChanged(index(0, ToAddress), index(priv->size()-1, ToAddress));
}

int TransactionTableModel::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return priv->size();
}

int TransactionTableModel::columnCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return columns.length();
}

QString TransactionTableModel::formatTxStatus(const TransactionRecord *wtx) const
{
    QString status;

    switch(wtx->status.status)
    {
    case TransactionStatus::OpenUntilBlock:
        status = tr("Open for %n more block(s)","",wtx->status.open_for);
        break;
    case TransactionStatus::OpenUntilDate:
        status = tr("Open until %1").arg(GUIUtil::dateTimeStr(wtx->status.open_for));
        break;
    case TransactionStatus::Offline:
        status = tr("Offline");
        break;
    case TransactionStatus::Unconfirmed:
        status = tr("Unconfirmed");
        break;
    case TransactionStatus::Abandoned:
        status = tr("Abandoned");
        break;
    case TransactionStatus::Confirming:
        status = tr("Confirming (%1 of %2 recommended confirmations)").arg(wtx->status.depth).arg(TransactionRecord::RecommendedNumConfirmations);
        break;
    case TransactionStatus::Confirmed:
        status = tr("Confirmed (%1 confirmations)").arg(wtx->status.depth);
        break;
    case TransactionStatus::Conflicted:
        status = tr("Conflicted");
        break;
    case TransactionStatus::Immature:
        status = tr("Immature (%1 confirmations, will be available after %2)").arg(wtx->status.depth).arg(wtx->status.depth + wtx->status.matures_in);
        break;
    case TransactionStatus::MaturesWarning:
        status = tr("This block was not received by any other nodes and will probably not be accepted!");
        break;
    case TransactionStatus::NotAccepted:
        status = tr("Generated but not accepted");
        break;
    }

    return status;
}

QString TransactionTableModel::formatTxDate(const TransactionRecord *wtx) const
{
    if(wtx->time)
    {
        return GUIUtil::dateTimeStr(wtx->time);
    }
    return QString();
}

/* Look up address in address book, if found return label (address)
   otherwise just return (address)
 */
QString TransactionTableModel::lookupAddress(const std::string &address, bool tooltip) const
{
    if (address.empty())
    {
        return tr("External payee");
    }
    if (address.find(", ") != std::string::npos)
    {
        return tr("Multiple addresses");
    }
    QString label = walletModel->getAddressTableModel()->labelForAddress(QString::fromStdString(address));
    QString description;
    if(!label.isEmpty())
    {
        description += label;
    }
    if(label.isEmpty() || tooltip)
    {
        description += QString(" (") + QString::fromStdString(address) + QString(")");
    }
    return description;
}

QString TransactionTableModel::formatTxType(const TransactionRecord *wtx) const
{
    switch(wtx->type)
    {
    case TransactionRecord::RecvWithAddress:
        return tr("Received with");
    case TransactionRecord::RecvFromOther:
        return tr("Received from");
    case TransactionRecord::SendToAddress:
    case TransactionRecord::SendToOther:
        return tr("Sent to");
    case TransactionRecord::SendToSelf:
        return tr("Payment to yourself");
    case TransactionRecord::InternalTransfer:
        return tr("Internal transfer");
    case TransactionRecord::Generated:
        return tr("Mining reward");
    case TransactionRecord::GeneratedWitness:
        return tr("Witness reward");
    case TransactionRecord::WitnessRenew:
        return tr("Renew witness account");
    case TransactionRecord::WitnessFundSend:
        return tr("Fund witness account");
    case TransactionRecord::WitnessFundRecv:
        return tr("Lock funds");
    case TransactionRecord::WitnessEmptySend:
        return tr("Empty witness account");
    case TransactionRecord::WitnessEmptyRecv:
        return tr("Received from witness account");
    case TransactionRecord::Other:
        return "";
    }
    return "";
}

QString TransactionTableModel::txAddressDecoration(const TransactionRecord *wtx) const
{
    switch(wtx->type)
    {
        case TransactionRecord::Generated:
            return "\uf0c1";
        case TransactionRecord::GeneratedWitness:
            return "\uf0c1";
        case TransactionRecord::RecvWithAddress:
        case TransactionRecord::RecvFromOther:
            return "\uf2f5";
        case TransactionRecord::SendToAddress:
        case TransactionRecord::SendToOther:
            return "\uf2f6";
        case TransactionRecord::WitnessFundSend:
        case TransactionRecord::WitnessFundRecv:
            return "\uf023";
        case TransactionRecord::WitnessEmptySend:
        case TransactionRecord::WitnessEmptyRecv:
            return "\uf09c";
        case TransactionRecord::WitnessRenew:
            return "\uf2f9";
        case TransactionRecord::SendToSelf:
        case TransactionRecord::InternalTransfer:
            return "\uf074";
        case TransactionRecord::Other:
            return "\uf362";
    }
    return "";
}

QString TransactionTableModel::formatTxToAddress(const TransactionRecord *wtx, bool tooltip) const
{
    QString watchAddress;
    if (tooltip) {
        // Mark transactions involving watch-only addresses by adding " (watch-only)"
        watchAddress = wtx->involvesWatchAddress ? QString(" (") + tr("watch-only") + QString(")") : "";
    }

    LOCK(wallet->cs_wallet);

    //fixme: (2.1) fShowChildAccountsSeperately
    if (wtx->credit > wtx->debit)
    {
        if ( wtx->fromAccountUUID != boost::uuids::nil_generator()())
        {
            boost::uuids::uuid fromUUID = wtx->fromAccountUUID;
            if (!fShowChildAccountsSeperately && wtx->fromAccountParentUUID != boost::uuids::nil_generator()())
            {
                fromUUID = wtx->fromAccountParentUUID;
            }
            //return tr("Self payment.");
            if (wallet->mapAccountLabels.count(fromUUID) != 0)
            {
                switch(wtx->type)
                {
                    case TransactionRecord::WitnessFundRecv:
                        return tr("Lock funds from: %1").arg(QString::fromStdString(wallet->mapAccountLabels[fromUUID]));
                    case TransactionRecord::WitnessEmptyRecv:
                        return tr("Unlock funds from: %1").arg(QString::fromStdString(wallet->mapAccountLabels[fromUUID]));
                    case TransactionRecord::SendToSelf:
                        break; // Fall through to bottom of function where this is handled.
                    default:
                        return tr("Internal transfer from: %1").arg(QString::fromStdString(wallet->mapAccountLabels[fromUUID]));
                }
            }
        }
    }
    else if (wtx->debit > wtx->credit)
    {
        if ( wtx->receiveAccountUUID != boost::uuids::nil_generator()())
        {
            boost::uuids::uuid receiveUUID = wtx->receiveAccountUUID;
            if (!fShowChildAccountsSeperately && wtx->receiveAccountParentUUID != boost::uuids::nil_generator()())
            {
                receiveUUID = wtx->receiveAccountParentUUID;
            }
            //return tr("Self payment.");
            if (wallet->mapAccountLabels.count(receiveUUID) != 0)
            {
                switch(wtx->type)
                {
                    case TransactionRecord::WitnessFundSend:
                        return tr("Fund witness account: %1").arg(QString::fromStdString(wallet->mapAccountLabels[receiveUUID]));
                    case TransactionRecord::WitnessEmptySend:
                        return tr("Unlock funds to: %1").arg(QString::fromStdString(wallet->mapAccountLabels[receiveUUID]));
                    case TransactionRecord::SendToSelf:
                        break; // Fall through to bottom of function where this is handled.
                    default:
                        return tr("Internal transfer to: %1").arg(QString::fromStdString(wallet->mapAccountLabels[receiveUUID]));
                }
            }
        }
    }

    switch(wtx->type)
    {
    case TransactionRecord::RecvFromOther:
        return QString::fromStdString(wtx->address) + watchAddress;
    case TransactionRecord::RecvWithAddress:
        return tr("Payment from: %1").arg(lookupAddress(wtx->address, tooltip) + watchAddress);
    case TransactionRecord::Generated:
        return tr("Mining reward") /*: + lookupAddress(wtx->address, tooltip) + watchAddress*/;
    case TransactionRecord::GeneratedWitness:
        return tr("Witness reward") /*: + lookupAddress(wtx->address, tooltip) + watchAddress*/;
    case TransactionRecord::WitnessRenew:
        return tr("Renew witness account");
    case TransactionRecord::SendToAddress:
        return tr("Paid to: %1").arg(lookupAddress(wtx->address, tooltip) + watchAddress);
    case TransactionRecord::SendToOther:
        return tr("Paid to: %1").arg(QString::fromStdString(wtx->address) + watchAddress);
    case TransactionRecord::SendToSelf:
        return tr("Internal account movement");
    case TransactionRecord::WitnessEmptySend:
    case TransactionRecord::WitnessEmptyRecv:
    case TransactionRecord::WitnessFundRecv:
    case TransactionRecord::WitnessFundSend:
    case TransactionRecord::InternalTransfer:
        return "";//Already  handled above this switch
    case TransactionRecord::Other:
        return tr("Complex transaction, view transaction details.") + watchAddress;
    }
    return "";
}

QVariant TransactionTableModel::addressColor(const TransactionRecord *wtx) const
{
    // Show addresses without label in a less visible color
    switch(wtx->type)
    {
        case TransactionRecord::RecvWithAddress:
        case TransactionRecord::SendToAddress:
        case TransactionRecord::Generated:
        case TransactionRecord::GeneratedWitness:
        case TransactionRecord::WitnessRenew:
        case TransactionRecord::WitnessFundSend:
        case TransactionRecord::WitnessFundRecv:
        case TransactionRecord::WitnessEmptySend:
        case TransactionRecord::WitnessEmptyRecv:
        case TransactionRecord::SendToSelf:
        case TransactionRecord::InternalTransfer:
            return COLOR_BAREADDRESS;
        default:
            break;
    }
    return QVariant();
}

QString TransactionTableModel::formatTxAmountReceived(const TransactionRecord *wtx, bool showUnconfirmed, GuldenUnits::SeparatorStyle separators) const
{
    QString str = GuldenUnits::format(walletModel->getOptionsModel()->getDisplayUnit(), wtx->credit , false, separators, 2);
    //fixme: (2.1) We could maybe strip the trailing .00 here to clean display up a bit?
    if(showUnconfirmed)
    {
        if(!wtx->status.countsForBalance)
        {
            str = QString("[") + str + QString("]");
        }
    }
    return QString(str);
}

QString TransactionTableModel::formatTxAmountSent(const TransactionRecord *wtx, bool showUnconfirmed, GuldenUnits::SeparatorStyle separators) const
{
    QString str = GuldenUnits::format(walletModel->getOptionsModel()->getDisplayUnit(), wtx->debit, false, separators, 2);
    if(showUnconfirmed)
    {
        if(!wtx->status.countsForBalance)
        {
            str = QString("[") + str + QString("]");
        }
    }
    return QString(str);
}

QString TransactionTableModel::txStatusDecoration(const TransactionRecord *wtx) const
{
    switch(wtx->status.status)
    {
        case TransactionStatus::Offline:
            return "\uf1e6";
        case TransactionStatus::Unconfirmed:
            return "\uf254";
        case TransactionStatus::Confirmed:
        case TransactionStatus::Confirming:
            switch(wtx->status.depth)
            {
            case 1: return GUIUtil::fontAwesomeLight("\uf132");
            case 2: return GUIUtil::fontAwesomeLight("\uf2f7");
            case 3: return GUIUtil::fontAwesomeRegular("\uf2f7");
            case 4:
            default:
                return GUIUtil::fontAwesomeSolid("\uf2f7");
            };
        case TransactionStatus::OpenUntilBlock:
        case TransactionStatus::OpenUntilDate:
        case TransactionStatus::Immature:
            return "\uf017";
        case TransactionStatus::Abandoned:
        case TransactionStatus::Conflicted:
        case TransactionStatus::MaturesWarning:
        case TransactionStatus::NotAccepted:
            return "\uf05e";
    }
    return "";
}

QVariant TransactionTableModel::txWatchonlyDecoration(const TransactionRecord *wtx) const
{
    if (wtx->involvesWatchAddress)
        return QIcon(":/icons/eye");
    else
        return QVariant();
}

QString TransactionTableModel::formatTooltip(const TransactionRecord *rec) const
{
    QString sType = formatTxType(rec);
    QString tooltip = formatTxStatus(rec);
    if (!sType.isEmpty())
        tooltip += QString("\n") + sType;
    if(rec->type==TransactionRecord::RecvFromOther || rec->type==TransactionRecord::SendToOther ||
       rec->type==TransactionRecord::SendToAddress || rec->type==TransactionRecord::RecvWithAddress)
    {
        tooltip += QString(" ") + formatTxToAddress(rec, true);
    }
    return tooltip;
}

QVariant TransactionTableModel::data(const QModelIndex &index, int role) const
{
    if(!index.isValid())
        return QVariant();
    TransactionRecord *rec = static_cast<TransactionRecord*>(index.internalPointer());

    switch(role)
    {
    case SortRole:
        switch(index.column())
        {
        case Date:
            return rec->time;
        case Type:
            return formatTxType(rec);
        case ToAddress:
            return formatTxToAddress(rec, false);
        case AmountReceived:
            return formatTxAmountReceived(rec, true, GuldenUnits::separatorAlways);
        case AmountSent:
            return formatTxAmountSent(rec, true, GuldenUnits::separatorAlways);
        }
        break;
    case RawDecorationRole:
        switch(index.column())
        {
        case Watchonly:
            return txWatchonlyDecoration(rec);
        }
        break;
    case Qt::DecorationRole:
    {
        QIcon icon = qvariant_cast<QIcon>(index.data(RawDecorationRole));
        return icon;
    }
    case Qt::DisplayRole:
        switch(index.column())
        {
        case Status:
            return txStatusDecoration(rec);
        case Date:
            return formatTxDate(rec);
        case Type:
            return formatTxType(rec);
        case ToAddress:
            return "<tr><td width=20 align=center>" + txAddressDecoration(rec) + "</td><td>" + formatTxToAddress(rec, false) + "</td>";
        case AmountReceived:
            return formatTxAmountReceived(rec, true, GuldenUnits::separatorAlways);
        case AmountSent:
            return formatTxAmountSent(rec, true, GuldenUnits::separatorAlways);
        }
        break;
    case Qt::EditRole:
        // Edit role is used for sorting, so return the unformatted values
        switch(index.column())
        {
        case Status:
            return QString::fromStdString(rec->status.sortKey);
        case Date:
            return rec->time;
        case Type:
            return formatTxType(rec);
        case Watchonly:
            return (rec->involvesWatchAddress ? 1 : 0);
        case ToAddress:
            return formatTxToAddress(rec, true);
        case AmountReceived:
            return qint64(rec->credit);
        case AmountSent:
            return qint64(rec->debit);
        }
        break;
    case Qt::ToolTipRole:
        return formatTooltip(rec);
    case Qt::TextAlignmentRole:
        return column_alignments[index.column()];
    case Qt::ForegroundRole:
        // Use the "danger" color for abandoned transactions
        if(rec->status.status == TransactionStatus::Abandoned)
        {
            return COLOR_TX_STATUS_DANGER;
        }
        // Non-confirmed (but not immature) as transactions are grey
        if(!rec->status.countsForBalance && rec->status.status != TransactionStatus::Immature)
        {
            return COLOR_UNCONFIRMED;
        }
        /*if(index.column() == Amount && (rec->credit+rec->debit) < 0)
        {
            return COLOR_NEGATIVE;
        }*/
        if(index.column() == ToAddress)
        {
            return addressColor(rec);
        }
        break;
    case TypeRole:
        return rec->type;
    case DateRole:
        return QDateTime::fromTime_t(static_cast<uint>(rec->time));
    case WatchonlyRole:
        return rec->involvesWatchAddress;
    case WatchonlyDecorationRole:
        return txWatchonlyDecoration(rec);
    case LongDescriptionRole:
        return priv->describe(rec, walletModel->getOptionsModel()->getDisplayUnit());
    case AddressRole:
        return QString::fromStdString(rec->address);
    case AccountRole:
        return QVariant::fromValue(rec->actionAccountUUID);
    case AccountParentRole:
        return QVariant::fromValue(rec->actionAccountParentUUID);
    case LabelRole:
        return walletModel->getAddressTableModel()->labelForAddress(QString::fromStdString(rec->address));
    case AmountRole:
        return qint64(rec->credit + rec->debit);
    case TxIDRole:
        return rec->getTxID();
    case TxHashRole:
        return QString::fromStdString(rec->hash.ToString());
    case TxHexRole:
        return priv->getTxHex(rec);
    case TxBlockHeightRole:
        return priv->getTxBlockNumber(rec);
    case TxPlainTextRole:
        {
            QString details;
            QDateTime date = QDateTime::fromTime_t(static_cast<uint>(rec->time));
            QString txLabel = walletModel->getAddressTableModel()->labelForAddress(QString::fromStdString(rec->address));

            details.append(date.toString("M/d/yy HH:mm"));
            details.append(" ");
            details.append(formatTxStatus(rec));
            details.append(". ");
            if(!formatTxType(rec).isEmpty()) {
                details.append(formatTxType(rec));
                details.append(" ");
            }
            if(!rec->address.empty()) {
                if(txLabel.isEmpty())
                    details.append(tr("(no label)") + " ");
                else {
                    details.append("(");
                    details.append(txLabel);
                    details.append(") ");
                }
                details.append(QString::fromStdString(rec->address));
                details.append(" ");
            }
            details.append(formatTxAmountReceived(rec, false, GuldenUnits::separatorNever));
            return details;
        }
    case ConfirmedRole:
        return rec->status.countsForBalance;
    case FormattedAmountRole:
        // Used for copy/export, so don't include separators
        return formatTxAmountReceived(rec, false, GuldenUnits::separatorNever);
    case StatusRole:
        return rec->status.status;
    case DepthRole:
        return rec->status.depth;
    }
    return QVariant();
}

QVariant TransactionTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if(orientation == Qt::Horizontal)
    {
        if(role == Qt::DisplayRole)
        {
            return columns[section];
        }
        else if (role == Qt::TextAlignmentRole)
        {
            return column_alignments[section];
        } else if (role == Qt::ToolTipRole)
        {
            switch(section)
            {
            case Status:
                return tr("Transaction status. Hover over this field to show number of confirmations.");
            case Date:
                return tr("Date and time that the transaction was received.");
            case Type:
                return tr("Type of transaction.");
            case Watchonly:
                return tr("Whether or not a watch-only address is involved in this transaction.");
            case ToAddress:
                return tr("User-defined intent/purpose of the transaction.");
            case AmountReceived:
                return tr("Amount added to balance.");
            case AmountSent:
                return tr("Amount removed from balance.");
            }
        }
    }
    return QVariant();
}

QModelIndex TransactionTableModel::index(int row, int column, const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    TransactionRecord *data = priv->index(row);
    if(data)
    {
        return createIndex(row, column, priv->index(row));
    }
    return QModelIndex();
}

void TransactionTableModel::updateDisplayUnit()
{
    // emit dataChanged to update Amount column with the current unit
    updateAmountColumnTitle();
    Q_EMIT dataChanged(index(0, AmountReceived), index(priv->size()-1, AmountReceived));
    Q_EMIT dataChanged(index(0, AmountSent), index(priv->size()-1, AmountSent));
}

// queue notifications to show a non freezing progress dialog e.g. for rescan
struct TransactionNotification
{
public:
    TransactionNotification() {}
    TransactionNotification(uint256 _hash, ChangeType _status, bool _showTransaction):
        hash(_hash), status(_status), showTransaction(_showTransaction) {}

    void invoke(QObject *ttm)
    {
        QString strHash = QString::fromStdString(hash.GetHex());
        qDebug() << "NotifyTransactionChanged: " + strHash + " status= " + QString::number(status);
        QMetaObject::invokeMethod(ttm, "updateTransaction", Qt::QueuedConnection,
                                  Q_ARG(QString, strHash),
                                  Q_ARG(int, status),
                                  Q_ARG(bool, showTransaction));
    }
private:
    uint256 hash;
    ChangeType status;
    bool showTransaction;
};

static bool fQueueNotifications = false;
static std::vector< TransactionNotification > vQueueNotifications;

static void NotifyTransactionChanged(TransactionTableModel *ttm, CWallet *wallet, const uint256 &hash, ChangeType status)
{
    // Find transaction in wallet
    std::map<uint256, CWalletTx>::iterator mi = wallet->mapWallet.find(hash);
    // Determine whether to show transaction or not (determine this here so that no relocking is needed in GUI thread)
    bool inWallet = mi != wallet->mapWallet.end();
    bool showTransaction = (inWallet && TransactionRecord::showTransaction(mi->second));

    TransactionNotification notification(hash, status, showTransaction);

    if (fQueueNotifications)
    {
        vQueueNotifications.push_back(notification);
        return;
    }
    notification.invoke(ttm);
}

static void ShowProgress(TransactionTableModel *ttm, const std::string &title, int nProgress)
{
    if (nProgress == 0)
        fQueueNotifications = true;

    if (nProgress == 100)
    {
        fQueueNotifications = false;
        if (vQueueNotifications.size() > 10) // prevent balloon spam, show maximum 10 balloons
            QMetaObject::invokeMethod(ttm, "setProcessingQueuedTransactions", Qt::QueuedConnection, Q_ARG(bool, true));
        for (unsigned int i = 0; i < vQueueNotifications.size(); ++i)
        {
            if (vQueueNotifications.size() - i <= 10)
                QMetaObject::invokeMethod(ttm, "setProcessingQueuedTransactions", Qt::QueuedConnection, Q_ARG(bool, false));

            vQueueNotifications[i].invoke(ttm);
        }
        std::vector<TransactionNotification >().swap(vQueueNotifications); // clear
    }
}

void TransactionTableModel::subscribeToCoreSignals()
{
    // Connect signals to wallet
    wallet->NotifyTransactionChanged.connect(boost::bind(NotifyTransactionChanged, this, _1, _2, _3));
    wallet->ShowProgress.connect(boost::bind(ShowProgress, this, _1, _2));
}

void TransactionTableModel::unsubscribeFromCoreSignals()
{
    // Disconnect signals from wallet
    wallet->NotifyTransactionChanged.disconnect(boost::bind(NotifyTransactionChanged, this, _1, _2, _3));
    wallet->ShowProgress.disconnect(boost::bind(ShowProgress, this, _1, _2));
}
