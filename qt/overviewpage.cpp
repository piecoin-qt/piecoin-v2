#include "overviewpage.h"
#include "ui_overviewpage.h"

#include "../main.h"
#include "walletmodel.h"
#include "bitcoinunits.h"
#include "optionsmodel.h"
#include "transactiontablemodel.h"
#include "transactionfilterproxy.h"
#include "guiutil.h"
#include "guiconstants.h"

#include <QAbstractItemDelegate>
#include <QPainter>
#include <QLabel>
#include <QFrame>
#include <QStaticText>
#include <QFontDatabase>
#include <QTimer>

#if QT_VERSION >= 0x050000
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QVariantMap>
#endif


#define DECORATION_SIZE 64
#define NUM_ITEMS 4

class TxViewDelegate : public QAbstractItemDelegate
{
    Q_OBJECT
public:
    TxViewDelegate(): QAbstractItemDelegate(), unit(BitcoinUnits::BTC)
    {
    }
    inline void paint(QPainter *painter, const QStyleOptionViewItem &option,
                      const QModelIndex &index ) const
    {
        painter->save();

        QIcon icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
        QRect mainRect = option.rect;
        QRect decorationRect(mainRect.topLeft(), QSize(DECORATION_SIZE, DECORATION_SIZE));
        int xspace = DECORATION_SIZE + 8;
        int ypad = 6;
        int halfheight = (mainRect.height() - 2*ypad)/2;
        QRect amountRect(mainRect.left() + xspace, mainRect.top()+ypad, mainRect.width() - xspace, halfheight);
        QRect addressRect(mainRect.left() + xspace, mainRect.top()+ypad+halfheight, mainRect.width() - xspace, halfheight);
        icon.paint(painter, decorationRect);

        QDateTime date = index.data(TransactionTableModel::DateRole).toDateTime();
        QString address = index.data(Qt::DisplayRole).toString();
        qint64 amount = index.data(TransactionTableModel::AmountRole).toLongLong();
        bool confirmed = index.data(TransactionTableModel::ConfirmedRole).toBool();
        QVariant value = index.data(Qt::ForegroundRole);
        QColor foreground = option.palette.color(QPalette::Text);
        if(qVariantCanConvert<QColor>(value))
        {
            foreground = qvariant_cast<QColor>(value);
        }

        painter->setPen(foreground);

        painter->drawText(addressRect, Qt::AlignLeft|Qt::AlignVCenter, address);

        if(amount < 0)
        {
            foreground = COLOR_NEGATIVE;
        }
        else if(!confirmed)
        {
            foreground = COLOR_UNCONFIRMED;
        }
        else
        {
            foreground = option.palette.color(QPalette::Text);
        }
        painter->setPen(foreground);
        QString amountText = BitcoinUnits::formatWithUnit(unit, amount, true);
        if(!confirmed)
        {
            amountText = QString("[") + amountText + QString("]");
        }
        painter->drawText(amountRect, Qt::AlignRight|Qt::AlignVCenter, amountText);

        painter->setPen(option.palette.color(QPalette::Text));
        painter->drawText(amountRect, Qt::AlignLeft|Qt::AlignVCenter, GUIUtil::dateTimeStr(date));

        painter->restore();
    }

    inline QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
    {
        return QSize(DECORATION_SIZE, DECORATION_SIZE);
    }

    int unit;
    int fontID;

};
#include "overviewpage.moc"

OverviewPage::OverviewPage(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::OverviewPage),
    currentBalance(0),
    currentStake(0),
    currentUnconfirmedBalance(-1),
    currentConfirmingBalance(-1),
    currentImmatureBalance(-1),
    txdelegate(new TxViewDelegate()),
    filter(0)
{
    ui->setupUi(this);


    txdelegate->fontID = -1;

    // HACK: Makes that label transparent for mouse events
    // Mitigates strange event swallowing behavior in main window
    ui->ov_recent_tx_label->setAttribute(Qt::WA_TransparentForMouseEvents);

    // Recent transactions
    ui->listTransactions->setItemDelegate(txdelegate);
    ui->listTransactions->setIconSize(QSize(DECORATION_SIZE, DECORATION_SIZE));
    ui->listTransactions->setMinimumHeight(NUM_ITEMS * (DECORATION_SIZE + 2));
    ui->listTransactions->setAttribute(Qt::WA_MacShowFocusRect, false);

    connect(ui->listTransactions, SIGNAL(clicked(QModelIndex)), this, SLOT(handleTransactionClicked(QModelIndex)));

    // init "out of sync" warning labels
    ui->labelWalletStatus->setText("(" + tr("out of sync") + ")");
    ui->labelTransactionsStatus->setText("(" + tr("out of sync") + ")");

    // start with displaying the "out of sync" warnings
    showOutOfSyncWarning(true);

#if QT_VERSION >= 0x050000
    // set a timer for price API
    nLastPrice = 0;
    nLastPriceUSD = 0;
    QTimer *timerPriceAPI = new QTimer();
    connect(timerPriceAPI, SIGNAL(timeout()), this, SLOT(sendRequest()));
    timerPriceAPI->start(90 * 1000);
#endif
}

void OverviewPage::handleTransactionClicked(const QModelIndex &index)
{
    if(filter)
        emit transactionClicked(filter->mapToSource(index));
}

OverviewPage::~OverviewPage()
{
    delete ui;
}

void OverviewPage::setBalance(qint64 balance, qint64 minted, qint64 stake, qint64 unconfirmedBalance, qint64 confirmingBalance, qint64 immatureBalance)
{
    int unit = model->getOptionsModel()->getDisplayUnit();
    currentBalance = balance;
    nTotalMinted = minted;
    currentStake = stake;
    currentUnconfirmedBalance = unconfirmedBalance;
    currentConfirmingBalance = confirmingBalance;
    currentImmatureBalance = immatureBalance;
    ui->labelBalance->setText(BitcoinUnits::formatWithUnit(unit, balance, false, 2));
    ui->labelTotalMinted->setText(BitcoinUnits::formatWithUnit(unit, minted, false, 2));
    ui->labelStake->setText(BitcoinUnits::formatWithUnit(unit, stake, false, 2));
    ui->labelUnconfirmed->setText(BitcoinUnits::formatWithUnit(unit, unconfirmedBalance, false, 2));
    ui->labelImmature->setText(BitcoinUnits::formatWithUnit(unit, immatureBalance, false, 2));
    ui->labelTotal->setText(BitcoinUnits::formatWithUnit(unit, balance + stake + unconfirmedBalance + immatureBalance, false, 2));


    if (confirmingBalance > unconfirmedBalance)
    {
            ui->ov_unconfirmed_label->setText("Confirming");
            ui->labelUnconfirmed->setText(BitcoinUnits::formatWithUnit(unit, confirmingBalance, false, 2));
    } else {
            if (ui->ov_unconfirmed_label->text() != "Unconfirmed")
                ui->ov_unconfirmed_label->setText("Unconfirmed");
    }

    // only show immature (newly mined) balance if it's non-zero, so as not to complicate things
    // for the non-mining users
    bool showUnconfirmed = false;

    if(confirmingBalance !=0 || unconfirmedBalance !=0)
        showUnconfirmed = true;

    bool showImmature = immatureBalance != 0;

    ui->ov_unconfirmed_label->setVisible(showUnconfirmed);
    ui->labelUnconfirmed->setVisible(showUnconfirmed);
    ui->labelImmature->setVisible(showImmature);
    ui->ov_immature_label->setVisible(showImmature);

#if QT_VERSION >= 0x050000
    sendRequest();
#else
	ui->labelBtcValueText->setVisible(false);
	ui->labelBtcValue->setVisible(false);
#endif
}

void OverviewPage::setModel(WalletModel *model)
{
    this->model = model;
    if(model && model->getOptionsModel())
    {
        // Set up transaction list
        filter = new TransactionFilterProxy();
        filter->setSourceModel(model->getTransactionTableModel());
        filter->setLimit(NUM_ITEMS);
        filter->setDynamicSortFilter(true);
        filter->setSortRole(Qt::EditRole);
        filter->setShowInactive(false);
        filter->sort(TransactionTableModel::Status, Qt::DescendingOrder);

        ui->listTransactions->setModel(filter);
        ui->listTransactions->setModelColumn(TransactionTableModel::ToAddress);

        // Keep up to date with wallet
        setBalance(model->getBalance(), model->getTotalMinted(), model->getStake(), model->getUnconfirmedBalance(), model->getConfirmingBalance(), model->getImmatureBalance());
        connect(model, SIGNAL(balanceChanged(qint64, qint64, qint64, qint64, qint64, qint64)), this, SLOT(setBalance(qint64, qint64, qint64, qint64, qint64, qint64)));

        connect(model->getOptionsModel(), SIGNAL(displayUnitChanged(int)), this, SLOT(updateDisplayUnit()));
    }

    // update the display unit, to not use the default ("BTC")
    updateDisplayUnit();

}

void OverviewPage::updateDisplayUnit()
{
    if(model && model->getOptionsModel())
    {
        if(currentBalance != -1)
            setBalance(currentBalance, nTotalMinted, model->getStake(), currentUnconfirmedBalance, currentConfirmingBalance, currentImmatureBalance);

        // Update txdelegate->unit with the current unit
        txdelegate->unit = model->getOptionsModel()->getDisplayUnit();

        ui->listTransactions->update();
    }
}

void OverviewPage::showOutOfSyncWarning(bool fShow)
{
    ui->labelWalletStatus->setVisible(fShow);
    ui->labelTransactionsStatus->setVisible(fShow);
}

uint nLastPriceCheck = 0;
// #if QT_VERSION >= 0x050000
void OverviewPage::updateBtcValueLabel(double nPrice, double nPriceUSD)
{
    ui->labelBtcValue->setText(QString::number(nPrice * currentBalance / COIN, 'f', 6) + " BTC / $" + QString::number(nPriceUSD * currentBalance / COIN, 'f', 2) + " USD");
}

QNetworkAccessManager *manager = new QNetworkAccessManager();

void OverviewPage::sendRequest()
{
    //only update the price once every 60 seconds, don't want to call the API too many times
    uint nTimeNow = QDateTime::currentDateTime().toUTC().toTime_t();
    if(nTimeNow - nLastPriceCheck < 60)
        updateBtcValueLabel(nLastPrice, nLastPriceUSD);
    nLastPriceCheck = nTimeNow;

    // decide how to process finished() signal
    connect(manager,SIGNAL(finished(QNetworkReply*)),this,SLOT(handlePriceReply(QNetworkReply*)));

    //build our URL to send GET request to
    QString strURL = "https://api.coingecko.com/api/v3/coins/piecoin?localization=false&"
                     "tickers=false&market_data=true&community_data=false&"
                     "developer_data=false&sparkline=false";

    manager->get(QNetworkRequest(QUrl(strURL)));
}

void OverviewPage::handlePriceReply(QNetworkReply *reply)
{
    reply->deleteLater();

    if(reply->error() == QNetworkReply::NoError)
    {
        // Get the http status code
        int v = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (v >= 200 && v < 300) // Success
        {
            // Here we got the final reply
            QByteArray result = reply->readAll();

            //Parse the json
            QJsonDocument jsonResponse = QJsonDocument::fromJson(result);
            QJsonObject responseObject = jsonResponse.object();

            if (responseObject.contains("market_data") && responseObject["market_data"].isObject())
            {
                QJsonObject marketData = responseObject["market_data"].toObject();
                if(marketData.contains("current_price") && marketData["current_price"].isObject())
                {
                    QJsonObject currentPrice = marketData["current_price"].toObject();
                    if (
                           currentPrice.contains("btc") && currentPrice["usd"].isDouble() &&
                           currentPrice.contains("usd") && currentPrice["usd"].isDouble()
                    )
                    {
                        if (currentPrice["btc"].toDouble() > 0)
                            nLastPrice = currentPrice["btc"].toDouble();

                        if (currentPrice["usd"].toDouble() > 0)
                            nLastPriceUSD = currentPrice["usd"].toDouble();
                    }
                }
            }

            updateBtcValueLabel(nLastPrice, nLastPriceUSD);

            return;
        }
        else if (v >= 300 && v < 400) // Redirection
        {
            // Get the redirection url
            QUrl newUrl = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
            newUrl = reply->url().resolved(newUrl);

            QNetworkAccessManager *manager = reply->manager();
            QNetworkRequest redirection(newUrl);
            manager->get(redirection);

            return;
        }
    }
    else
        return;
}

// #endif
