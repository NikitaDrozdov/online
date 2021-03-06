/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 100 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "MessageQueue.hpp"

#include <algorithm>

#include <Poco/StringTokenizer.h>

#include <Protocol.hpp>
#include <Log.hpp>
#include <TileDesc.hpp>

using Poco::StringTokenizer;

MessageQueue::~MessageQueue()
{
    clear();
}

void MessageQueue::put(const Payload& value)
{
    std::unique_lock<std::mutex> lock(_mutex);
    put_impl(value);
    lock.unlock();
    _cv.notify_one();
}

MessageQueue::Payload MessageQueue::get(const unsigned timeoutMs)
{
    std::unique_lock<std::mutex> lock(_mutex);

    if (timeoutMs > 0)
    {
        if (!_cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                          [this] { return wait_impl(); }))
        {
            throw std::runtime_error("Timed out waiting to get queue item.");
        }
    }
    else
    {
        _cv.wait(lock, [this] { return wait_impl(); });
    }

    return get_impl();
}

void MessageQueue::clear()
{
    std::unique_lock<std::mutex> lock(_mutex);
    clear_impl();
}

void MessageQueue::remove_if(const std::function<bool(const Payload&)>& pred)
{
    std::unique_lock<std::mutex> lock(_mutex);
    std::remove_if(_queue.begin(), _queue.end(), pred);
}

void MessageQueue::put_impl(const Payload& value)
{
    const auto msg = std::string(value.data(), value.size());
    _queue.push_back(value);
}

bool MessageQueue::wait_impl() const
{
    return _queue.size() > 0;
}

MessageQueue::Payload MessageQueue::get_impl()
{
    Payload result = _queue.front();
    _queue.erase(_queue.begin());
    return result;
}

void MessageQueue::clear_impl()
{
    _queue.clear();
}

void TileQueue::put_impl(const Payload& value)
{
    const auto msg = std::string(value.data(), value.size());
    const std::string firstToken = LOOLProtocol::getFirstToken(value);

    if (firstToken == "canceltiles")
    {
        LOG_TRC("Processing [" << msg << "]. Before canceltiles have " << _queue.size() << " in queue.");
        const auto seqs = msg.substr(12);
        StringTokenizer tokens(seqs, ",", StringTokenizer::TOK_IGNORE_EMPTY | StringTokenizer::TOK_TRIM);
        _queue.erase(std::remove_if(_queue.begin(), _queue.end(),
                [&tokens](const Payload& v)
                {
                    const std::string s(v.data(), v.size());
                    // Tile is for a thumbnail, don't cancel it
                    if (s.find("id=") != std::string::npos)
                        return false;
                    for (size_t i = 0; i < tokens.count(); ++i)
                    {
                        if (s.find("ver=" + tokens[i]) != std::string::npos)
                        {
                            LOG_TRC("Matched " << tokens[i] << ", Removing [" << s << "]");
                            return true;
                        }
                    }

                    return false;

                }), _queue.end());

        // Don't push canceltiles into the queue.
        LOG_TRC("After canceltiles have " << _queue.size() << " in queue.");
        return;
    }
    else if (firstToken == "tilecombine")
    {
        // Breakup tilecombine and deduplicate (we are re-combining the tiles
        // in the get_impl() again)
        const auto tileCombined = TileCombined::parse(msg);
        for (auto& tile : tileCombined.getTiles())
        {
            const std::string newMsg = tile.serialize("tile");

            removeDuplicate(newMsg);

            MessageQueue::put_impl(Payload(newMsg.data(), newMsg.data() + newMsg.size()));
        }
        return;
    }
    else if (firstToken == "tile")
    {
        removeDuplicate(msg);

        MessageQueue::put_impl(value);
        return;
    }

    // TODO probably we could deduplacite the invalidation callbacks (later
    // one wins) the same way as we do for the tiles - to be tested.

    MessageQueue::put_impl(value);
}

void TileQueue::removeDuplicate(const std::string& tileMsg)
{
    assert(LOOLProtocol::matchPrefix("tile", tileMsg, /*ignoreWhitespace*/ true));

    // Ver is always provided at this point and it is necessary to
    // return back to clients the last rendered version of a tile
    // in case there are new invalidations and requests while rendering.
    // Here we compare duplicates without 'ver' since that's irrelevant.
    auto newMsgPos = tileMsg.find(" ver");
    if (newMsgPos == std::string::npos)
    {
        newMsgPos = tileMsg.size() - 1;
    }

    for (size_t i = 0; i < _queue.size(); ++i)
    {
        auto& it = _queue[i];
        if (it.size() > newMsgPos &&
            strncmp(tileMsg.data(), it.data(), newMsgPos) == 0)
        {
            LOG_TRC("Remove duplicate message: " << std::string(it.data(), it.size()) << " -> " << tileMsg);
            _queue.erase(_queue.begin() + i);
            break;
        }
    }
}

int TileQueue::priority(const std::string& tileMsg)
{
    auto tile = TileDesc::parse(tileMsg); //FIXME: Expensive, avoid.

    for (int i = static_cast<int>(_viewOrder.size()) - 1; i >= 0; --i)
    {
        auto& cursor = _cursorPositions[_viewOrder[i]];
        if (tile.intersectsWithRect(cursor.X, cursor.Y, cursor.Width, cursor.Height))
            return i;
    }

    return -1;
}

int TileQueue::findFirstNonPreview(bool preferTiles) const
{
    int firstTile = -1;
    int firstElse = -1;
    for (size_t i = 0; i < _queue.size(); ++i)
    {
        const auto& front = _queue[i];
        const bool isTile = LOOLProtocol::matchPrefix("tile", front);
        //LOG_WRN("#" << i << " " << (isTile ? "isTile" : "non-tile"));

        if (isTile && firstTile < 0)
        {
            const std::string msg(front.data(), front.size());
            std::string id;
            const bool isPreview = LOOLProtocol::getTokenStringFromMessage(msg, "id", id);
            //LOG_WRN("#" << i << " " << (isPreview ? "isPreview" : "isTile") << ": " << msg);
            if (!isPreview)
            {
                firstTile = i;
                //LOG_WRN("firstTile: #" << i);
            }
        }
        else if (!isTile && firstElse < 0)
        {
            firstElse = i;
            //LOG_WRN("firstElse: #" << i);
        }
        else if (firstTile >=0 && firstElse >= 0)
        {
            break;
        }
    }

    if (preferTiles && firstTile >= 0)
    {
        return firstTile;
    }

    if (firstElse >= 0)
    {
        return firstElse;
    }

    if (firstTile >= 0)
    {
        return firstTile;
    }

    return -1;
}

void TileQueue::bumpToTop(const size_t index)
{
    if (index > 0)
    {
        Payload payload(_queue[index]);
        //LOG_WRN("Bumping: " << std::string(payload.data(), payload.size()));

        _queue.erase(_queue.begin() + index);
        _queue.insert(_queue.begin(), payload);
    }
}

void TileQueue::updateTimestamps(const bool isTile)
{
    if (isTile)
    {
        _lastGetTile = true;
        _lastTileGetTime = std::chrono::steady_clock::now();
    }
    else if (_lastGetTile)
    {
        // Update non-tile timestamp when switching from tiles.
        _lastGetTile = false;
        _lastGetTime = std::chrono::steady_clock::now();
    }
}

bool TileQueue::shouldPreferTiles() const
{
    if (_lastGetTile)
    {
        // If we had just done a tile, do something else.
        LOG_TRC("Last was tile, doing non-tiles.");
        return false;
    }

    // Check how long it's been since we'd done tiles.
    const auto tileDuration = (_lastGetTime - _lastTileGetTime);
    const auto tileDurationMs = std::chrono::duration_cast<std::chrono::milliseconds>(tileDuration).count();
    const auto duration = (std::chrono::steady_clock::now() - _lastGetTime);
    const auto durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
    LOG_TRC("Tile duration: " << tileDurationMs << "ms, nonTile duration: " << durationMs << "ms.");

    if (durationMs > MaxTileSkipDurationMs)
    {
        LOG_TRC("Capping non-tiles to 100ms. Prefer tiles now.");
        return true;
    }

    if (durationMs > tileDurationMs)
    {
        LOG_TRC("Capping non-tiles to tileDurationMs (" << tileDurationMs << "). Prefer tiles now.");
        return true;
    }

    // We can still do some more non-tiles.
    LOG_TRC("Have time for more non-tiles.");
    return false;
}

MessageQueue::Payload TileQueue::get_impl()
{
    LOG_TRC("MessageQueue depth: " << _queue.size());

    auto front = _queue.front();
    bool isTileFirst = LOOLProtocol::matchPrefix("tile", front);
    //LOG_WRN("isTileFirst: " << isTileFirst);

    if (_queue.size() == 1)
    {
        updateTimestamps(isTileFirst);

        //const auto msg = std::string(front.data(), front.size());
        //LOG_TRC("MessageQueue res only: " << msg);
        _queue.erase(_queue.begin());
        return front;
    }

    // Drain callbacks as soon and as fast as possible.
    if (!isTileFirst && LOOLProtocol::matchPrefix("callback", front))
    {
        updateTimestamps(false);

        //const auto msg = std::string(front.data(), front.size());
        //LOG_TRC("MessageQueue res call: " << msg);
        _queue.erase(_queue.begin());
        return front;
    }

    // TODO: Try draining all callbacks first.

    const bool preferTiles = shouldPreferTiles();
    const int nonPreviewIndex = findFirstNonPreview(preferTiles);
    //LOG_WRN("First non-preview: " << nonPreviewIndex);
    if (nonPreviewIndex < 0)
    {
        // We are left with previews only.
        updateTimestamps(true); // We're doing a tile.

        //const auto msg = std::string(front.data(), front.size());
        //LOG_TRC("MessageQueue res prev: " << msg);
        _queue.erase(_queue.begin());
        return front;
    }

    bumpToTop(nonPreviewIndex);
    front = _queue.front();
    isTileFirst = LOOLProtocol::matchPrefix("tile", front);
    //LOG_WRN("New front: " << std::string(front.data(), front.size()));

    if (!isTileFirst)
    {
        updateTimestamps(false);

        //const auto msg = std::string(front.data(), front.size());
        //LOG_TRC("MessageQueue res call: " << msg);
        _queue.erase(_queue.begin());
        return front;
    }

    // We are handling a tile; first try to find one that is at the cursor's
    // position, otherwise handle the one that is at the front
    int prioritized = 0;
    int prioritySoFar = -1;
    std::string msg(front.data(), front.size());
    for (size_t i = 0; i < _queue.size(); ++i)
    {
        auto& it = _queue[i];
        const std::string prio(it.data(), it.size());

        // avoid starving - stop the search when we reach a non-tile,
        // otherwise we may keep growing the queue of unhandled stuff (both
        // tiles and non-tiles)
        std::string id;
        if (!LOOLProtocol::matchPrefix("tile", prio) ||
            LOOLProtocol::getTokenStringFromMessage(prio, "id", id))
        {
            break;
        }

        const int p = priority(prio);
        if (p > prioritySoFar)
        {
            prioritySoFar = p;
            prioritized = i;
            msg = prio;

            // found the highest priority already?
            if (prioritySoFar == static_cast<int>(_viewOrder.size()) - 1)
            {
                break;
            }
        }
    }

    _queue.erase(_queue.begin() + prioritized);

    std::vector<TileDesc> tiles;
    tiles.emplace_back(TileDesc::parse(msg));

    // Combine as many tiles as possible with the top one.
    for (size_t i = 0; i < _queue.size(); )
    {
        auto& it = _queue[i];
        msg = std::string(it.data(), it.size());
        std::string id;
        if (!LOOLProtocol::matchPrefix("tile", msg) ||
            LOOLProtocol::getTokenStringFromMessage(msg, "id", id))
        {
            // Don't combine non-tiles or tiles with id.
            ++i;
            continue;
        }

        auto tile2 = TileDesc::parse(msg);
        //LOG_TRC("Combining candidate: " << msg);

        // Check if it's on the same row.
        if (tiles[0].onSameRow(tile2))
        {
            tiles.emplace_back(tile2);
            _queue.erase(_queue.begin() + i);
        }
        else
        {
            ++i;
        }
    }

    updateTimestamps(true);

    LOG_TRC("Combined " << tiles.size() << " tiles, leaving " << _queue.size() << " in queue.");

    if (tiles.size() == 1)
    {
        msg = tiles[0].serialize("tile");
        LOG_TRC("MessageQueue res: " << msg);
        return Payload(msg.data(), msg.data() + msg.size());
    }

    const auto tileCombined = TileCombined::create(tiles).serialize("tilecombine");
    LOG_TRC("MessageQueue res: " << tileCombined);
    return Payload(tileCombined.data(), tileCombined.data() + tileCombined.size());
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
