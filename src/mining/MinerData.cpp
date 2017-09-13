﻿// ==========================================================================
// 
// creepMiner - Burstcoin cryptocurrency CPU and GPU miner
// Copyright (C)  2016-2017 Creepsky (creepsky@gmail.com)
// 
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 3 of the License, or
// (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software Foundation,
// Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110 - 1301  USA
// 
// ==========================================================================

#include "MinerData.hpp"
#include "MinerConfig.hpp"
#include "logging/MinerLogger.hpp"
#include "shabal/MinerShabal.hpp"
#include "logging/Output.hpp"
#include "MinerUtil.hpp"
#include "wallet/Wallet.hpp"
#include "wallet/Account.hpp"

Burst::BlockData::BlockData(Poco::UInt64 blockHeight, Poco::UInt64 baseTarget, std::string genSigStr, MinerData* parent)
	: blockHeight_ {blockHeight},
	  baseTarget_ {baseTarget},
	  genSigStr_ {genSigStr},
	  activityLastWinner_ {this, &BlockData::runGetLastWinner},
	  parent_{parent}
{
	entries_ = std::make_shared<std::vector<Poco::JSON::Object>>();
	//deadlines_ = std::make_shared<std::unordered_map<AccountId, Deadlines>>();

	for (auto i = 0; i < 32; ++i)
	{
		auto byteStr = genSigStr.substr(i * 2, 2);
		genSig_[i] = static_cast<uint8_t>(std::stoi(byteStr, nullptr, 16));
	}

	Shabal256 hash;
	GensigData newGenSig;

	hash.update(&genSig_[0], genSig_.size());
	hash.update(blockHeight);
	hash.close(&newGenSig[0]);

	scoop_ = (static_cast<int>(newGenSig[newGenSig.size() - 2] & 0x0F) << 8) | static_cast<int>(newGenSig[newGenSig.size() - 1]);

	refreshBlockEntry();
}

std::shared_ptr<Burst::Deadline> Burst::BlockData::addDeadlineUnlocked(Poco::UInt64 nonce, Poco::UInt64 deadline,
	std::shared_ptr<Burst::Account> account, Poco::UInt64 block, std::string plotFile)
{
	if (account == nullptr)
		return nullptr;

	auto accountId = account->getId();
	
	auto iter = deadlines_.find(accountId);

	if (iter == deadlines_.end())
	{
		deadlines_.insert(std::make_pair(accountId, Deadlines(this)));
		return deadlines_[accountId].add(nonce, deadline, account, block, plotFile);
	}

	return iter->second.add(nonce, deadline, account, block, plotFile);
}

std::shared_ptr<Burst::Deadline> Burst::BlockData::addDeadline(Poco::UInt64 nonce, Poco::UInt64 deadline, std::shared_ptr<Account> account, Poco::UInt64 block, std::string plotFile)
{
	std::lock_guard<std::mutex> lock{ mutex_ };
	return addDeadlineUnlocked(nonce, deadline, account, block, plotFile);
}

void Burst::BlockData::setBaseTarget(Poco::UInt64 baseTarget)
{
	baseTarget_ = baseTarget;
}

void Burst::BlockData::confirmedDeadlineEvent(std::shared_ptr<Deadline> deadline)
{
	if (deadline == nullptr)
		return;

	addBlockEntry(createJsonDeadline(*deadline, "nonce confirmed"));

	// set the best deadline for this block
	{
		std::lock_guard<std::mutex> lock{ mutex_ };

		if (bestDeadline_ == nullptr ||
			bestDeadline_->getDeadline() > deadline->getDeadline())
			bestDeadline_ = deadline;
	}

	// set the best deadline overall
	if (parent_ != nullptr)
	{
		parent_->setBestDeadline(deadline);
		parent_->addConfirmedDeadline();
	}
}

void Burst::BlockData::setLastWinner(std::shared_ptr<Account> account)
{
	// set the winner for the last block
	{
		std::lock_guard<std::mutex> lock{mutex_};
		lastWinner_ = account;
	}

	auto lastWinnerJson = account->toJSON();
	lastWinnerJson->set("type", "lastWinner");
	addBlockEntry(*lastWinnerJson);
}

void Burst::BlockData::refreshBlockEntry() const
{
	addBlockEntry(createJsonNewBlock(*parent_));
}

void Burst::BlockData::refreshConfig() const
{
	addBlockEntry(createJsonConfig());
}

void Burst::BlockData::refreshPlotDirs() const
{
	addBlockEntry(createJsonPlotDirsRescan());
}

void Burst::BlockData::setProgress(float progressRead, float progressVerification, Poco::UInt64 blockheight)
{
	if (blockheight != getBlockheight())
		return;

	{
		std::lock_guard<std::mutex> lock{ mutex_ };
		jsonProgress_ = new Poco::JSON::Object{createJsonProgress(progressRead, progressVerification)};
	}

	if (parent_ != nullptr)
		parent_->blockDataChangedEvent.notify(this, *jsonProgress_);
}

void Burst::BlockData::setProgress(const std::string& plotDir, float progress, Poco::UInt64 blockheight)
{
	if (blockheight != getBlockheight())
		return;

	std::lock_guard<std::mutex> lock{ mutex_ };
	auto json = new Poco::JSON::Object{ createJsonProgress(progress, 0.f) };
	json->set("type", "plotdir-progress");
	json->set("dir", plotDir);

	jsonDirProgress_[plotDir].assign(json);

	if (parent_ != nullptr)
		parent_->blockDataChangedEvent.notify(this, *json);
}

void Burst::BlockData::addBlockEntry(Poco::JSON::Object entry) const
{
	{
		std::lock_guard<std::mutex> lock{ mutex_ };
		entries_->emplace_back(entry);	
	}
	
	if (parent_ != nullptr)
		parent_->blockDataChangedEvent.notify(this, entry);
}

Poco::UInt64 Burst::BlockData::getBlockheight() const
{
	return blockHeight_.load();
}

Poco::UInt64 Burst::BlockData::getScoop() const
{
	return scoop_.load();
}

Poco::UInt64 Burst::BlockData::getBasetarget() const
{
	return baseTarget_.load();
}

std::shared_ptr<Burst::Account> Burst::BlockData::getLastWinner() const
{
	return lastWinner_;
}
 
Poco::ActiveResult<std::shared_ptr<Burst::Account>> Burst::BlockData::getLastWinnerAsync(const Wallet& wallet, const Accounts& accounts)
{
	auto tuple = std::make_pair(&wallet, &accounts);
	return activityLastWinner_(tuple);
}

const Burst::GensigData& Burst::BlockData::getGensig() const
{
	return genSig_;
}

const std::string& Burst::BlockData::getGensigStr() const
{
	return genSigStr_;
}

std::shared_ptr<Burst::Deadline> Burst::BlockData::getBestDeadline() const
{
	return bestDeadline_;
}

bool Burst::BlockData::forEntries(std::function<bool(const Poco::JSON::Object&)> traverseFunction) const
{
	std::lock_guard<std::mutex> lock{ mutex_ };

	if (entries_ == nullptr)
		return false;

	auto error = false;

	for (auto iter = entries_->begin(); iter != entries_->end() && !error; ++iter)
		error = !traverseFunction(*iter);

	// send the overall progress, if any
	if (!error && !jsonProgress_.isNull())
		error = !traverseFunction(*jsonProgress_);

	// send the dir progress
	if (!error)
		for (auto iter = jsonDirProgress_.begin(); !error && iter != jsonDirProgress_.end(); ++iter)
			error = !traverseFunction(*iter->second);

	return error;
}

//std::vector<Poco::JSON::Object> Burst::BlockData::getEntries() const
//{
//	Poco::ScopedLock<Poco::Mutex> lock{mutex_};
//	return *entries_;
//}

//const std::unordered_map<Burst::AccountId, Burst::Deadlines>& Burst::BlockData::getDeadlines() const
//{
//	Poco::ScopedLock<Poco::FastMutex> lock{mutex_};
//	return deadlines_;
//}

std::shared_ptr<Burst::Deadline> Burst::BlockData::getBestDeadlineUnlocked(Poco::UInt64 accountId, Burst::BlockData::DeadlineSearchType searchType)
{
	auto iter = deadlines_.find(accountId);

	if (iter == deadlines_.end())
		return nullptr;

	switch (searchType)
	{
	case DeadlineSearchType::Found:
		return iter->second.getBestFound();
	case DeadlineSearchType::Sent:
		return iter->second.getBestSent();
	case DeadlineSearchType::Confirmed:
		return iter->second.getBestConfirmed();
	default:
		return nullptr;
	}
}

std::shared_ptr<Burst::Deadline> Burst::BlockData::getBestDeadline(Poco::UInt64 accountId, BlockData::DeadlineSearchType searchType)
{
	std::lock_guard<std::mutex> lock{ mutex_ };
	return getBestDeadlineUnlocked(accountId, searchType);
}

std::shared_ptr<Burst::Deadline> Burst::BlockData::addDeadlineIfBest(Poco::UInt64 nonce, Poco::UInt64 deadline,
	std::shared_ptr<Account> account, Poco::UInt64 block, std::string plotFile)
{
	std::lock_guard<std::mutex> lock{ mutex_ };

	auto bestDeadline = getBestDeadlineUnlocked(account->getId(), DeadlineSearchType::Found);

	if (bestDeadline == nullptr || bestDeadline->getDeadline() > deadline)
		return addDeadlineUnlocked(nonce, deadline, account, block, plotFile);

	return nullptr;
}

void Burst::BlockData::addMessage(const Poco::Message& message) const
{
	Poco::JSON::Object json;

	{
		std::lock_guard<std::mutex> lock{ mutex_ };

		if (entries_ == nullptr)
			return;
		
		json.set("type", std::to_string(static_cast<int>(message.getPriority())));
		json.set("text", message.getText());
		json.set("source", message.getSource());
		json.set("line", message.getSourceLine());
		json.set("file", message.getSourceFile());
		json.set("time", Poco::DateTimeFormatter::format(Poco::LocalDateTime(message.getTime()), "%H:%M:%S"));

		entries_->emplace_back(json);
	}

	if (parent_ != nullptr)
		parent_->blockDataChangedEvent.notify(this, json);
}

void Burst::BlockData::clearEntries() const
{
	std::lock_guard<std::mutex> lock{ mutex_ };
	entries_->clear();
}

std::shared_ptr<Burst::Account> Burst::BlockData::runGetLastWinner(const std::pair<const Wallet*, const Accounts*>& args)
{
	poco_ndc(Miner::runGetLastWinner);

	AccountId lastWinner;
	auto lastBlockheight = blockHeight_ - 1;
	auto& wallet = *args.first;
	auto& accounts = *args.second;

	if (!wallet.isActive())
		return nullptr;

	if (wallet.getWinnerOfBlock(lastBlockheight, lastWinner))
	{
		// we are the winner :)
		if (accounts.isLoaded(lastWinner))
		{
			parent_->addWonBlock();
			// we (re)send the good news to the local html server
			addBlockEntry(createJsonNewBlock(*parent_));
		}

		auto winnerAccount = std::make_shared<Account>(wallet, lastWinner);
		auto futureName = winnerAccount->getNameAsync();
		futureName.wait();

		log_ok_if(MinerLogger::miner, MinerLogger::hasOutput(LastWinner), std::string(50, '-') + "\n"
			"last block winner: \n"
			"block#             %Lu\n"
			"winner-numeric     %Lu\n"
			"winner-address     %s\n"
			"%s" +
			std::string(50, '-'),
			lastBlockheight, lastWinner, winnerAccount->getAddress(),
			(winnerAccount->getName().empty() ? "" : Poco::format("winner-name        %s\n", winnerAccount->getName()))
		);

		setLastWinner(winnerAccount);

		return winnerAccount;
	}

	return nullptr;
}

Burst::MinerData::MinerData()
	: blocksMined_(0), blocksWon_(0), deadlinesConfirmed_(0), targetDeadline_(0),
	currentBlockheight_(0), currentBasetarget_(0), currentScoopNum_(0)
{}

std::shared_ptr<Burst::BlockData> Burst::MinerData::startNewBlock(Poco::UInt64 block, Poco::UInt64 baseTarget, const std::string& genSig)
{
	std::unique_lock<std::mutex> lock{ mutex_ };

	// save the old data in the historical container
	if (blockData_ != nullptr)
	{
		const auto maxSize = 30;

		// if we reached the maximum size of blocks, forget the oldest
		if (historicalBlocks_.size() + 1 > maxSize)
			historicalBlocks_.pop_front();

		// we clear all entries, because it is also in the logfile
		// and we dont need it in the ram anymore
		blockData_->clearEntries();

		historicalBlocks_.emplace_back(blockData_);

		++blocksMined_;
	}

	lock.unlock();
	blockData_ = std::make_shared<BlockData>(block, baseTarget, genSig, this);

	lock.lock();
	currentBlockheight_ = block;
	currentBasetarget_ = baseTarget;
	currentScoopNum_ = blockData_->getScoop();

	return blockData_;
}

void Burst::MinerData::addConfirmedDeadline()
{
	++deadlinesConfirmed_;
}

void Burst::MinerData::setBestDeadline(std::shared_ptr<Deadline> deadline)
{
	std::lock_guard<std::mutex> lock{ mutex_ };

	// we check if its the best deadline overall
	if (bestDeadlineOverall_ == nullptr ||
		deadline->getDeadline() < bestDeadlineOverall_->getDeadline())
		bestDeadlineOverall_ = deadline;
}

void Burst::MinerData::setTargetDeadline(Poco::UInt64 deadline)
{
	targetDeadline_ = deadline;
}

void Burst::MinerData::addMessage(const Poco::Message& message)
{
	auto blockData = getBlockData();

	if (blockData != nullptr)
		blockData->addMessage(message);
}

void Burst::MinerData::addWonBlock()
{
	++blocksWon_;
}

std::shared_ptr<Burst::Deadline> Burst::MinerData::getBestDeadlineOverall() const
{
	return bestDeadlineOverall_;
}

const Poco::Timestamp& Burst::MinerData::getStartTime() const
{
	return startTime_;
}

Poco::Timespan Burst::MinerData::getRunTime() const
{
	return Poco::Timestamp{} - getStartTime();
}

Poco::UInt64 Burst::MinerData::getBlocksMined() const
{
	return blocksMined_;
}

Poco::UInt64 Burst::MinerData::getBlocksWon() const
{
	return blocksWon_;
}

std::shared_ptr<Burst::BlockData> Burst::MinerData::getBlockData()
{
	return blockData_;
}

std::shared_ptr<const Burst::BlockData> Burst::MinerData::getBlockData() const
{
	return blockData_;
}

//std::shared_ptr<const Poco::JSON::Object> Burst::MinerData::getLastWinner() const
//{
//	Poco::ScopedLock<Poco::Mutex> lock{mutex_};
//
//	// we make a local copy here, because the block data could be changed
//	// every second and we would have undefined behaviour
//	auto blockData = getBlockData();
//
//	if (blockData == nullptr)
//		return nullptr;
//
//	return blockData->getLastWinner();
//}

std::shared_ptr<const Burst::BlockData> Burst::MinerData::getHistoricalBlockData(Poco::UInt32 roundsBefore) const
{
	if (roundsBefore == 0)
		return getBlockData();

	auto wantedBlock = getBlockData()->getBlockheight() - roundsBefore;

	std::lock_guard<std::mutex> lock{ mutex_ };

	auto iter = std::find_if(historicalBlocks_.begin(), historicalBlocks_.end(), [wantedBlock](const std::shared_ptr<BlockData>& data)
	{
		return data->getBlockheight() == wantedBlock;
	});

	if (iter == historicalBlocks_.end())
		return nullptr;

	return (*iter);
}

std::vector<std::shared_ptr<const Burst::BlockData>> Burst::MinerData::getAllHistoricalBlockData() const
{
	std::lock_guard<std::mutex> lock{ mutex_ };
	
	std::vector<std::shared_ptr<const BlockData>> blocks;
	blocks.reserve(historicalBlocks_.size());

	for (auto& block : historicalBlocks_)
		blocks.emplace_back(block);

	return blocks;
}

Poco::UInt64 Burst::MinerData::getConfirmedDeadlines() const
{
	return deadlinesConfirmed_;
}

Poco::UInt64 Burst::MinerData::getTargetDeadline(TargetDeadlineType type) const
{
	switch (type)
	{
	case TargetDeadlineType::Pool:
		return targetDeadline_.load();
	case TargetDeadlineType::Local:
		return MinerConfig::getConfig().getTargetDeadline();
	case TargetDeadlineType::Combined:
		{
			auto targetDeadline = targetDeadline_.load();
			auto manualTargetDeadline = MinerConfig::getConfig().getTargetDeadline();

			if (targetDeadline == 0)
				targetDeadline = manualTargetDeadline;
			else if (targetDeadline > manualTargetDeadline &&
				manualTargetDeadline > 0)
				targetDeadline = manualTargetDeadline;

			return targetDeadline;
		}
	default:
		return 0;
	}
}

bool Burst::MinerData::compareToTargetDeadline(Poco::UInt64 deadline) const
{
	return deadline < getTargetDeadline();
}

Poco::UInt64 Burst::MinerData::getAverageDeadline() const
{
	std::lock_guard<std::mutex> lock{ mutex_ };
	
	Poco::UInt64 avg = 0;
	size_t size = 0;

	if (historicalBlocks_.empty())
		return 0;

	for (const auto& block : historicalBlocks_)
	{
		const auto bestDeadline = block->getBestDeadline();

		if (bestDeadline != nullptr)
		{
			avg += bestDeadline->getDeadline();
			++size;
		}
	}

	if (size == 0)
		return 0;

	return avg / size;
}

Poco::UInt64 Burst::MinerData::getCurrentBlockheight() const
{
	return currentBlockheight_.load();
}

Poco::UInt64 Burst::MinerData::getCurrentBasetarget() const
{
	return currentBasetarget_.load();
}

Poco::UInt64 Burst::MinerData::getCurrentScoopNum() const
{
	return currentScoopNum_.load();
}
