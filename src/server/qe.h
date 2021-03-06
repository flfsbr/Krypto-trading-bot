#ifndef K_QE_H_
#define K_QE_H_

namespace K {
  class QE: public Klass {
    private:
      map<mQuotingMode, function<mQuote(double, double, double)>*> quotingMode;
      mQuoteState bidStatus = mQuoteState::MissingData,
                  askStatus = mQuoteState::MissingData;
      mQuoteStatus status;
      unsigned int AK47inc = 1;
    public:
      mConnectivity gwConnectButton   = mConnectivity::Disconnected,
                    gwConnectExchange = mConnectivity::Disconnected;
    protected:
      void load() {
        quotingMode[mQuotingMode::Top]         = &calcTopOfMarket;
        quotingMode[mQuotingMode::Mid]         = &calcMidOfMarket;
        quotingMode[mQuotingMode::Join]        = &calcJoinMarket;
        quotingMode[mQuotingMode::InverseJoin] = &calcInverseJoinMarket;
        quotingMode[mQuotingMode::InverseTop]  = &calcInverseTopOfMarket;
        quotingMode[mQuotingMode::HamelinRat]  = &calcColossusOfMarket;
        quotingMode[mQuotingMode::Depth]       = &calcDepthOfMarket;
      };
      void waitTime() {
        ((EV*)events)->tEngine->setData(this);
        ((EV*)events)->tEngine->start([](Timer *tEngine) {
          ((QE*)tEngine->getData())->timer_1s();
        }, 1e+3, 1e+3);
      };
      void waitData() {
        ((EV*)events)->uiQuotingParameters = [&]() {                _debugEvent_
          ((MG*)market)->calcFairValue();
          ((PG*)wallet)->calcTargetBasePos();
          ((PG*)wallet)->calcSafety();
          ((MG*)market)->calcEwmaHistory();
          calcQuote();
        };
        ((EV*)events)->mgEwmaQuoteProtection = [&]() {              _debugEvent_
          calcQuote();
        };
        ((EV*)events)->mgLevels = [&]() {                           _debugEvent_
          calcQuote();
        };
        ((EV*)events)->pgTargetBasePosition = [&]() {               _debugEvent_
          calcQuote();
        };
      };
      void waitUser() {
        ((UI*)client)->welcome(mMatter::QuoteStatus, &hello);
      };
      void run() {
        if (((CF*)config)->argDebugQuotes) return;
        debuq = [&](string k, mQuote rawQuote) {};
        debug = [&](string k) {};
      };
    private:
      function<void(json*)> hello = [&](json *welcome) {
        *welcome = { status };
      };
      void timer_1s() {                                             _debugEvent_
        if (((MG*)market)->fairValue) {
          ((MG*)market)->calcStats();
          ((PG*)wallet)->calcSafety();
          calcQuote();
        } else FN::logWar("QE", "Unable to calculate quote, missing market data");
      };
      void calcQuote() {
        bidStatus = mQuoteState::MissingData;
        askStatus = mQuoteState::MissingData;
        if (!gwConnectExchange) {
          bidStatus = mQuoteState::Disconnected;
          askStatus = mQuoteState::Disconnected;
        } else if (((MG*)market)->fairValue and !((MG*)market)->levels.empty()) {
          if (!gwConnectButton) {
            bidStatus = mQuoteState::DisabledQuotes;
            askStatus = mQuoteState::DisabledQuotes;
            stopAllQuotes(mSide::Both);
          } else {
            bidStatus = mQuoteState::UnknownHeld;
            askStatus = mQuoteState::UnknownHeld;
            sendQuoteToAPI();
          }
        }
        sendStatusToUI();
      };
      void sendQuoteToAPI() {
        mQuote quote = nextQuote();
        bidStatus = checkCrossedQuotes(mSide::Bid, &quote);
        askStatus = checkCrossedQuotes(mSide::Ask, &quote);
        if (askStatus == mQuoteState::Live) updateQuote(quote.ask, mSide::Ask, quote.isAskPong);
        else stopAllQuotes(mSide::Ask);
        if (bidStatus == mQuoteState::Live) updateQuote(quote.bid, mSide::Bid, quote.isBidPong);
        else stopAllQuotes(mSide::Bid);
      };
      void sendStatusToUI() {
        unsigned int quotesInMemoryNew = 0;
        unsigned int quotesInMemoryWorking = 0;
        unsigned int quotesInMemoryDone = 0;
        bool k = diffCounts(&quotesInMemoryNew, &quotesInMemoryWorking, &quotesInMemoryDone);
        if (!diffStatus() and !k) return;
        status = mQuoteStatus(bidStatus, askStatus, quotesInMemoryNew, quotesInMemoryWorking, quotesInMemoryDone);
        ((UI*)client)->send(mMatter::QuoteStatus, status);
      };
      bool diffCounts(unsigned int *qNew, unsigned int *qWorking, unsigned int *qDone) {
        ((MG*)market)->filterBidOrders.clear();
        ((MG*)market)->filterAskOrders.clear();
        vector<string> zombies;
        unsigned long now = _Tstamp_;
        for (map<string, mOrder>::value_type &it : ((OG*)broker)->orders)
          if (it.second.orderStatus == mStatus::New) {
            if (now-10e+3>it.second.time) zombies.push_back(it.second.orderId);
            (*qNew)++;
          } else if (it.second.orderStatus == mStatus::Working) {
            (mSide::Bid == it.second.side
              ? ((MG*)market)->filterBidOrders
              : ((MG*)market)->filterAskOrders
            )[it.second.price] += it.second.quantity;
            (*qWorking)++;
          } else (*qDone)++;
        for (string &it : zombies) ((OG*)broker)->cleanOrder(it);
        return *qNew != status.quotesInMemoryNew
          or *qWorking != status.quotesInMemoryWorking
          or *qDone != status.quotesInMemoryDone;
      };
      bool diffStatus() {
        return bidStatus != status.bidStatus
          or askStatus != status.askStatus;
      };
      mQuote nextQuote() {
        if (((MG*)market)->levels.empty() or ((PG*)wallet)->position.empty()) return mQuote();
        double baseValue       = ((PG*)wallet)->position.baseValue,
               baseAmount      = ((PG*)wallet)->position.baseAmount,
               baseHeldAmount  = ((PG*)wallet)->position.baseHeldAmount,
               quoteAmount     = ((PG*)wallet)->position.quoteAmount,
               quoteHeldAmount = ((PG*)wallet)->position.quoteHeldAmount,
               safetyBuyPing   = ((PG*)wallet)->safety.buyPing,
               safetysellPing  = ((PG*)wallet)->safety.sellPing,
               safetyBuy       = ((PG*)wallet)->safety.buy,
               safetySell      = ((PG*)wallet)->safety.sell,
               pDiv            = ((PG*)wallet)->positionDivergence;
        if (safetyBuyPing == -1) return mQuote();
        double totalBasePosition = baseAmount + baseHeldAmount;
        double totalQuotePosition = (quoteAmount + quoteHeldAmount) / ((MG*)market)->fairValue;
        double widthPing = qp->widthPercentage
          ? qp->widthPingPercentage * ((MG*)market)->fairValue / 100
          : qp->widthPing;
        double widthPong = qp->widthPercentage
          ? qp->widthPongPercentage * ((MG*)market)->fairValue / 100
          : qp->widthPong;
        double buySize = qp->percentageValues
          ? qp->buySizePercentage * baseValue / 100
          : qp->buySize;
        double sellSize = qp->percentageValues
          ? qp->sellSizePercentage * baseValue / 100
          : qp->sellSize;
        if (qp->aggressivePositionRebalancing != mAPR::Off) {
          if (buySize and qp->buySizeMax)
            buySize = fmax(buySize, ((PG*)wallet)->targetBasePosition - totalBasePosition);
          if (sellSize and qp->sellSizeMax)
            sellSize = fmax(sellSize, totalBasePosition - ((PG*)wallet)->targetBasePosition);
        }
        if(qp->protectionEwmaWidthPing and ((MG*)market)->mgEwmaW)
          widthPing = fmax(widthPing, ((MG*)market)->mgEwmaW);
        mQuote rawQuote = quote(widthPing, buySize, sellSize);
        if (!rawQuote.bid.price and !rawQuote.ask.price) return mQuote();
        if (rawQuote.bid.price < 0 or rawQuote.ask.price < 0) {
          FN::logWar("QP", "Negative price detected, widthPing or/and widthPong must be smaller");
          return mQuote();
        }
        const double rawBidSz = rawQuote.bid.size;
        const double rawAskSz = rawQuote.ask.size;
        bool superTradesActive = false;
        debuq("?", rawQuote); applySuperTrades(&rawQuote, &superTradesActive, widthPing, buySize, sellSize, quoteAmount, baseAmount);
        debuq("A", rawQuote); applyEwmaProtection(&rawQuote);
        debuq("B", rawQuote); applyTotalBasePosition(&rawQuote, totalBasePosition, pDiv, buySize, sellSize, quoteAmount, baseAmount);
        debuq("C", rawQuote); applyStdevProtection(&rawQuote);
        debuq("D", rawQuote); applyAggressivePositionRebalancing(&rawQuote, widthPong, safetyBuyPing, safetysellPing);
        debuq("E", rawQuote); applyAK47Increment(&rawQuote, baseValue);
        debuq("F", rawQuote); applyBestWidth(&rawQuote);
        debuq("G", rawQuote); applyTradesPerMinute(&rawQuote, superTradesActive, safetyBuy, safetySell);
        debuq("H", rawQuote); applyRoundPrice(&rawQuote);
        debuq("I", rawQuote); applyRoundSize(&rawQuote, rawBidSz, rawAskSz, totalQuotePosition, totalBasePosition);
        debuq("J", rawQuote); applyDepleted(&rawQuote, totalQuotePosition, totalBasePosition);
        debuq("K", rawQuote); applyWaitingPing(&rawQuote, safetyBuyPing, safetysellPing);
        debuq("!", rawQuote);
        rawQuote.isAskPong = (safetyBuyPing and rawQuote.ask.price and rawQuote.ask.price >= safetyBuyPing + widthPong);
        rawQuote.isBidPong = (safetysellPing and rawQuote.bid.price and rawQuote.bid.price <= safetysellPing - widthPong);
        debug(string("totals ") + "toAsk:" + to_string(totalBasePosition) + ", toBid:" + to_string(totalQuotePosition));
        return rawQuote;
      };
      void applyRoundPrice(mQuote *rawQuote) {
        if (rawQuote->bid.price) {
          rawQuote->bid.price = floor(rawQuote->bid.price / gw->minTick) * gw->minTick;
          rawQuote->bid.price = fmax(0, rawQuote->bid.price);
        }
        if (rawQuote->ask.price) {
          rawQuote->ask.price = ceil(rawQuote->ask.price / gw->minTick) * gw->minTick;
          rawQuote->ask.price = fmax(rawQuote->bid.price + gw->minTick, rawQuote->ask.price);
        }
      };
      void applyRoundSize(mQuote *rawQuote, const double rawBidSz, const double rawAskSz, double totalQuotePosition, double totalBasePosition) {
        if (rawQuote->ask.size) {
          if (rawQuote->ask.size > totalBasePosition)
            rawQuote->ask.size = (!rawBidSz or rawBidSz > totalBasePosition)
              ? totalBasePosition : rawBidSz;
          rawQuote->ask.size = floor(fmax(gw->minSize, rawQuote->ask.size) / 1e-8) * 1e-8;
        }
        if (rawQuote->bid.size) {
          if (rawQuote->bid.size > totalQuotePosition)
            rawQuote->bid.size = (!rawAskSz or rawAskSz > totalQuotePosition)
              ? totalQuotePosition : rawAskSz;
          rawQuote->bid.size = floor(fmax(gw->minSize, rawQuote->bid.size) / 1e-8) * 1e-8;
        }
      };
      void applyWaitingPing(mQuote *rawQuote, double safetyBuyPing, double safetysellPing) {
        if (!qp->_matchPings and qp->safety != mQuotingSafety::PingPong) return;
        if (!safetyBuyPing and (
          (bidStatus != mQuoteState::DepletedFunds and (qp->pingAt == mPingAt::DepletedSide or qp->pingAt == mPingAt::DepletedBidSide))
          or qp->pingAt == mPingAt::StopPings
          or qp->pingAt == mPingAt::BidSide
          or qp->pingAt == mPingAt::DepletedAskSide
        )) {
          askStatus = mQuoteState::WaitingPing;
          rawQuote->ask.price = 0;
          rawQuote->ask.size = 0;
        }
        if (!safetysellPing and (
          (askStatus != mQuoteState::DepletedFunds and (qp->pingAt == mPingAt::DepletedSide or qp->pingAt == mPingAt::DepletedAskSide))
          or qp->pingAt == mPingAt::StopPings
          or qp->pingAt == mPingAt::AskSide
          or qp->pingAt == mPingAt::DepletedBidSide
        )) {
          bidStatus = mQuoteState::WaitingPing;
          rawQuote->bid.price = 0;
          rawQuote->bid.size = 0;
        }
      };
      void applyDepleted(mQuote *rawQuote, double totalQuotePosition, double totalBasePosition) {
        if (rawQuote->bid.size and rawQuote->bid.size > totalQuotePosition) {
          bidStatus = mQuoteState::DepletedFunds;
          rawQuote->bid.price = 0;
          rawQuote->bid.size = 0;
        }
        if (rawQuote->ask.size and rawQuote->ask.size > totalBasePosition) {
          askStatus = mQuoteState::DepletedFunds;
          rawQuote->ask.price = 0;
          rawQuote->ask.size = 0;
        }
      };
      void applyBestWidth(mQuote *rawQuote) {
        if (!qp->bestWidth) return;
        if (rawQuote->ask.price)
          for (mLevel &it : ((MG*)market)->levels.asks)
            if (it.price > rawQuote->ask.price) {
              double bestAsk = it.price - gw->minTick;
              if (bestAsk > ((MG*)market)->fairValue) {
                rawQuote->ask.price = bestAsk;
                break;
              }
            }
        if (rawQuote->bid.price)
          for (mLevel &it : ((MG*)market)->levels.bids)
            if (it.price < rawQuote->bid.price) {
              double bestBid = it.price + gw->minTick;
              if (bestBid < ((MG*)market)->fairValue) {
                rawQuote->bid.price = bestBid;
                break;
              }
            }
      };
      void applyTotalBasePosition(mQuote *rawQuote, double totalBasePosition, double pDiv, double buySize, double sellSize, double quoteAmount, double baseAmount) {
        if (rawQuote->ask.price and totalBasePosition < ((PG*)wallet)->targetBasePosition - pDiv) {
          askStatus = mQuoteState::TBPHeld;
          rawQuote->ask.price = 0;
          rawQuote->ask.size = 0;
          if (qp->aggressivePositionRebalancing != mAPR::Off) {
            ((PG*)wallet)->sideAPR = "Buy";
            if (!qp->buySizeMax) rawQuote->bid.size = fmin(qp->aprMultiplier*buySize, fmin(((PG*)wallet)->targetBasePosition - totalBasePosition, (quoteAmount / ((MG*)market)->fairValue) / 2));
          }
        }
        else if (rawQuote->bid.price and totalBasePosition >= ((PG*)wallet)->targetBasePosition + pDiv) {
          bidStatus = mQuoteState::TBPHeld;
          rawQuote->bid.price = 0;
          rawQuote->bid.size = 0;
          if (qp->aggressivePositionRebalancing != mAPR::Off) {
            ((PG*)wallet)->sideAPR = "Sell";
            if (!qp->sellSizeMax) rawQuote->ask.size = fmin(qp->aprMultiplier*sellSize, fmin(totalBasePosition - ((PG*)wallet)->targetBasePosition, baseAmount / 2));
          }
        }
        else ((PG*)wallet)->sideAPR = "Off";
      };
      void applyTradesPerMinute(mQuote *rawQuote, bool superTradesActive, double safetyBuy, double safetySell) {
        if (safetySell > (qp->tradesPerMinute * (superTradesActive ? qp->sopWidthMultiplier : 1))) {
          askStatus = mQuoteState::MaxTradesSeconds;
          rawQuote->ask.price = 0;
          rawQuote->ask.size = 0;
        }
        if (safetyBuy > (qp->tradesPerMinute * (superTradesActive ? qp->sopWidthMultiplier : 1))) {
          bidStatus = mQuoteState::MaxTradesSeconds;
          rawQuote->bid.price = 0;
          rawQuote->bid.size = 0;
        }
      };
      void applyAggressivePositionRebalancing(mQuote *rawQuote, double widthPong, double safetyBuyPing, double safetysellPing) {
        if (qp->safety == mQuotingSafety::Boomerang or qp->safety == mQuotingSafety::AK47 or qp->_matchPings) {
          if (rawQuote->ask.size and safetyBuyPing and (
            (qp->aggressivePositionRebalancing == mAPR::SizeWidth and ((PG*)wallet)->sideAPR == "Sell")
            or qp->pongAt == mPongAt::ShortPingAggressive
            or qp->pongAt == mPongAt::LongPingAggressive
            or rawQuote->ask.price < safetyBuyPing + widthPong
          )) rawQuote->ask.price = safetyBuyPing + widthPong;
          if (rawQuote->bid.size and safetysellPing and (
            (qp->aggressivePositionRebalancing == mAPR::SizeWidth and ((PG*)wallet)->sideAPR == "Buy")
            or qp->pongAt == mPongAt::ShortPingAggressive
            or qp->pongAt == mPongAt::LongPingAggressive
            or rawQuote->bid.price > safetysellPing - widthPong
          )) rawQuote->bid.price = safetysellPing - widthPong;
        }
      };
      void applySuperTrades(mQuote *rawQuote, bool *superTradesActive, double widthPing, double buySize, double sellSize, double quoteAmount, double baseAmount) {
        if (qp->superTrades != mSOP::Off
          and widthPing * qp->sopWidthMultiplier < ((MG*)market)->levels.asks.begin()->price - ((MG*)market)->levels.bids.begin()->price
        ) {
          *superTradesActive = (qp->superTrades == mSOP::Trades or qp->superTrades == mSOP::TradesSize);
          if (qp->superTrades == mSOP::Size or qp->superTrades == mSOP::TradesSize) {
            if (!qp->buySizeMax) rawQuote->bid.size = fmin(qp->sopSizeMultiplier * buySize, (quoteAmount / ((MG*)market)->fairValue) / 2);
            if (!qp->sellSizeMax) rawQuote->ask.size = fmin(qp->sopSizeMultiplier * sellSize, baseAmount / 2);
          }
        }
      };
      void applyAK47Increment(mQuote *rawQuote, double baseValue) {
        if (qp->safety != mQuotingSafety::AK47) return;
        double range = qp->percentageValues
          ? qp->rangePercentage * baseValue / 100
          : qp->range;
        rawQuote->bid.price -= AK47inc * range;
        rawQuote->ask.price += AK47inc * range;
        if (++AK47inc > qp->bullets) AK47inc = 1;
      };
      void applyStdevProtection(mQuote *rawQuote) {
        if (qp->quotingStdevProtection == mSTDEV::Off or !((MG*)market)->mgStdevFV) return;
        if (rawQuote->ask.price and (qp->quotingStdevProtection == mSTDEV::OnFV or qp->quotingStdevProtection == mSTDEV::OnTops or qp->quotingStdevProtection == mSTDEV::OnTop or ((PG*)wallet)->sideAPR != "Sell"))
          rawQuote->ask.price = fmax(
            (qp->quotingStdevBollingerBands
              ? (qp->quotingStdevProtection == mSTDEV::OnFV or qp->quotingStdevProtection == mSTDEV::OnFVAPROff)
                ? ((MG*)market)->mgStdevFVMean : ((qp->quotingStdevProtection == mSTDEV::OnTops or qp->quotingStdevProtection == mSTDEV::OnTopsAPROff)
                  ? ((MG*)market)->mgStdevTopMean : ((MG*)market)->mgStdevAskMean )
              : ((MG*)market)->fairValue) + ((qp->quotingStdevProtection == mSTDEV::OnFV or qp->quotingStdevProtection == mSTDEV::OnFVAPROff)
                ? ((MG*)market)->mgStdevFV : ((qp->quotingStdevProtection == mSTDEV::OnTops or qp->quotingStdevProtection == mSTDEV::OnTopsAPROff)
                  ? ((MG*)market)->mgStdevTop : ((MG*)market)->mgStdevAsk )),
            rawQuote->ask.price
          );
        if (rawQuote->bid.price and (qp->quotingStdevProtection == mSTDEV::OnFV or qp->quotingStdevProtection == mSTDEV::OnTops or qp->quotingStdevProtection == mSTDEV::OnTop or ((PG*)wallet)->sideAPR != "Buy"))
          rawQuote->bid.price = fmin(
            (qp->quotingStdevBollingerBands
              ? (qp->quotingStdevProtection == mSTDEV::OnFV or qp->quotingStdevProtection == mSTDEV::OnFVAPROff)
                ? ((MG*)market)->mgStdevFVMean : ((qp->quotingStdevProtection == mSTDEV::OnTops or qp->quotingStdevProtection == mSTDEV::OnTopsAPROff)
                  ? ((MG*)market)->mgStdevTopMean : ((MG*)market)->mgStdevBidMean )
              : ((MG*)market)->fairValue) - ((qp->quotingStdevProtection == mSTDEV::OnFV or qp->quotingStdevProtection == mSTDEV::OnFVAPROff)
                ? ((MG*)market)->mgStdevFV : ((qp->quotingStdevProtection == mSTDEV::OnTops or qp->quotingStdevProtection == mSTDEV::OnTopsAPROff)
                  ? ((MG*)market)->mgStdevTop : ((MG*)market)->mgStdevBid )),
            rawQuote->bid.price
          );
      };
      void applyEwmaProtection(mQuote *rawQuote) {
        if (!qp->protectionEwmaQuotePrice or !((MG*)market)->mgEwmaP) return;
        rawQuote->ask.price = fmax(((MG*)market)->mgEwmaP, rawQuote->ask.price);
        rawQuote->bid.price = fmin(((MG*)market)->mgEwmaP, rawQuote->bid.price);
      };
      mQuote quote(double widthPing, double buySize, double sellSize) {
        if (quotingMode.find(qp->mode) == quotingMode.end())
          exit(_errorEvent_("QE", "Invalid quoting mode"));
        return (*quotingMode[qp->mode])(widthPing, buySize, sellSize);
      };
      mQuote quoteAtTopOfMarket() {
        mLevel topBid = ((MG*)market)->levels.bids.begin()->size > gw->minTick
          ? ((MG*)market)->levels.bids.at(0) : ((MG*)market)->levels.bids.at(((MG*)market)->levels.bids.size()>1?1:0);
        mLevel topAsk = ((MG*)market)->levels.asks.begin()->size > gw->minTick
          ? ((MG*)market)->levels.asks.at(0) : ((MG*)market)->levels.asks.at(((MG*)market)->levels.asks.size()>1?1:0);
        return mQuote(topBid, topAsk);
      };
      function<mQuote(double, double, double)> calcJoinMarket = [&](double widthPing, double buySize, double sellSize) {
        mQuote k = quoteAtTopOfMarket();
        k.bid.price = fmin(((MG*)market)->fairValue - widthPing / 2.0, k.bid.price);
        k.ask.price = fmax(((MG*)market)->fairValue + widthPing / 2.0, k.ask.price);
        k.bid.size = buySize;
        k.ask.size = sellSize;
        return k;
      };
      function<mQuote(double, double, double)> calcTopOfMarket = [&](double widthPing, double buySize, double sellSize) {
        mQuote k = quoteAtTopOfMarket();
        k.bid.price = k.bid.price + gw->minTick;
        k.ask.price = k.ask.price - gw->minTick;
        k.bid.price = fmin(((MG*)market)->fairValue - widthPing / 2.0, k.bid.price);
        k.ask.price = fmax(((MG*)market)->fairValue + widthPing / 2.0, k.ask.price);
        k.bid.size = buySize;
        k.ask.size = sellSize;
        return k;
      };
      function<mQuote(double, double, double)> calcInverseJoinMarket = [&](double widthPing, double buySize, double sellSize) {
        mQuote k = quoteAtTopOfMarket();
        double mktWidth = abs(k.ask.price - k.bid.price);
        if (mktWidth > widthPing) {
          k.ask.price = k.ask.price + widthPing;
          k.bid.price = k.bid.price - widthPing;
        }
        if (mktWidth < (2.0 * widthPing / 3.0)) {
          k.ask.price = k.ask.price + widthPing / 4.0;
          k.bid.price = k.bid.price - widthPing / 4.0;
        }
        k.bid.size = buySize;
        k.ask.size = sellSize;
        return k;
      };
      function<mQuote(double, double, double)> calcInverseTopOfMarket = [&](double widthPing, double buySize, double sellSize) {
        mQuote k = quoteAtTopOfMarket();
        double mktWidth = abs(k.ask.price - k.bid.price);
        if (mktWidth > widthPing) {
          k.ask.price = k.ask.price + widthPing;
          k.bid.price = k.bid.price - widthPing;
        }
        k.bid.price = k.bid.price + gw->minTick;
        k.ask.price = k.ask.price - gw->minTick;
        if (mktWidth < (2.0 * widthPing / 3.0)) {
          k.ask.price = k.ask.price + widthPing / 4.0;
          k.bid.price = k.bid.price - widthPing / 4.0;
        }
        k.bid.size = buySize;
        k.ask.size = sellSize;
        return k;
      };
      function<mQuote(double, double, double)> calcMidOfMarket = [&](double widthPing, double buySize, double sellSize) {
        return mQuote(
          mLevel(fmax(((MG*)market)->fairValue - widthPing, 0), buySize),
          mLevel(((MG*)market)->fairValue + widthPing, sellSize)
        );
      };
      function<mQuote(double, double, double)> calcColossusOfMarket = [&](double widthPing, double buySize, double sellSize) {
        mQuote k = quoteAtTopOfMarket();
        k.bid.size = 0;
        k.ask.size = 0;
        for (mLevel &it : ((MG*)market)->levels.bids)
          if (k.bid.size < it.size and it.price <= k.bid.price) {
            k.bid.size = it.size;
            k.bid.price = it.price;
          }
        for (mLevel &it : ((MG*)market)->levels.asks)
          if (k.ask.size < it.size and it.price >= k.ask.price) {
            k.ask.size = it.size;
            k.ask.price = it.price;
          }
        if (k.bid.size) k.bid.price += gw->minTick;
        if (k.ask.size) k.ask.price -= gw->minTick;
        k.bid.size = buySize;
        k.ask.size = sellSize;
        return k;
      };
      function<mQuote(double, double, double)> calcDepthOfMarket = [&](double depth, double buySize, double sellSize) {
        double bidPx = ((MG*)market)->levels.bids.begin()->price;
        double bidDepth = 0;
        for (mLevel &it : ((MG*)market)->levels.bids) {
          bidDepth += it.size;
          if (bidDepth >= depth) break;
          else bidPx = it.price;
        }
        double askPx = ((MG*)market)->levels.asks.begin()->price;
        double askDepth = 0;
        for (mLevel &it : ((MG*)market)->levels.asks) {
          askDepth += it.size;
          if (askDepth >= depth) break;
          else askPx = it.price;
        }
        return mQuote(
          mLevel(bidPx, buySize),
          mLevel(askPx, sellSize)
        );
      };
      mQuoteState checkCrossedQuotes(mSide side, mQuote *quote) {
        bool cross = false;
        if (side == mSide::Bid) {
          if (!quote->bid.price) return bidStatus;
          if (!quote->ask.price) return mQuoteState::Live;
          cross = quote->bid.price >= quote->ask.price;
        } else if (side == mSide::Ask) {
          if (!quote->ask.price) return askStatus;
          if (!quote->bid.price) return mQuoteState::Live;
          cross = quote->ask.price <= quote->bid.price;
        }
        if (cross) {
          FN::logWar("QE", "Cross quote detected");
          return mQuoteState::Crossed;
        } else return mQuoteState::Live;
      };
      void updateQuote(mLevel q, mSide side, bool isPong) {
        unsigned int n = 0;
        vector<string> toCancel,
                       working;
        for (map<string, mOrder>::value_type &it : ((OG*)broker)->orders)
          if (it.second.side != side) continue;
          else if (abs(it.second.price - q.price) < gw->minTick) return;
          else if (it.second.orderStatus == mStatus::New) {
            if (qp->safety != mQuotingSafety::AK47 or ++n >= qp->bullets) return;
          } else if (qp->safety != mQuotingSafety::AK47 or (
            side == mSide::Bid ? q.price <= it.second.price : q.price >= it.second.price
          )) toCancel.push_back(it.second.orderId);
          else working.push_back(it.second.orderId);
        if (qp->safety == mQuotingSafety::AK47 and toCancel.empty() and !working.empty())
          toCancel.push_back(side == mSide::Bid ? working.front() : working.back());
        ((OG*)broker)->sendOrder(toCancel, side, q.price, q.size, mOrderType::Limit, mTimeInForce::GTC, isPong, true);
      };
      void stopAllQuotes(mSide side) {
        for (map<string, mOrder>::value_type &it : ((OG*)broker)->orders)
          if (it.second.orderStatus == mStatus::Working and (side == mSide::Both or side == it.second.side))
            ((OG*)broker)->cancelOrder(it.second.orderId);
      };
      function<void(string,mQuote)> debuq = [&](string k, mQuote rawQuote) {
        debug(string("quote") + k + " " + to_string((int)bidStatus) + " " + to_string((int)askStatus) + " " + ((json)rawQuote).dump());
      };
      function<void(string)> debug = [&](string k) {
        FN::log("DEBUG", string("QE ") + k);
      };
  };
}

#endif
