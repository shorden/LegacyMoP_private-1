/*
 * Copyright (C) 2008-2012 TrinityCore <http://www.trinitycore.org/>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */


#include "ChallengeMgr.h"
#include "QueryResult.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "Player.h"
#include "ObjectAccessor.h"

ChallengeMgr::~ChallengeMgr()
{
    for(ChallengeMap::iterator itr = m_ChallengeMap.begin(); itr != m_ChallengeMap.end(); ++itr)
        delete itr->second;

    m_ChallengeMap.clear();
    m_ChallengesOfMember.clear();
    m_BestForMap.clear();
}

void ChallengeMgr::CheckBestMapId(Challenge *c)
{
    if (!m_BestForMap[c->mapID] || m_BestForMap[c->mapID]->recordTime > c->recordTime)
        m_BestForMap[c->mapID] = c;
}

void ChallengeMgr::CheckBestGuildMapId(Challenge *c)
{
    if (!c->guildId)
        return;

    if (!m_GuildBest[c->guildId][c->mapID] || m_GuildBest[c->guildId][c->mapID]->recordTime > c->recordTime)
        m_GuildBest[c->guildId][c->mapID] = c;
}

void ChallengeMgr::CheckBestMemberMapId(uint64 guid, Challenge *c)
{
    if (!m_ChallengesOfMember[guid][c->mapID] || m_ChallengesOfMember[guid][c->mapID]->recordTime > c->recordTime)
        m_ChallengesOfMember[guid][c->mapID] = c;
}

void ChallengeMgr::SaveChallengeToDB(Challenge *c)
{
    SQLTransaction trans = CharacterDatabase.BeginTransaction();

    PreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_CHALLENGE);
    stmt->setUInt32(0, c->Id);
    stmt->setUInt16(1, c->mapID);
    stmt->setUInt32(2, c->recordTime);
    stmt->setUInt32(3, c->date);
    stmt->setUInt8(4, c->medal);
    trans->Append(stmt);

    for(ChallengeMemberList::const_iterator itr = c->member.begin(); itr != c->member.end(); ++itr)
    {
        stmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_CHALLENGE_MEMBER);
        stmt->setUInt32(0, c->Id);
        stmt->setUInt64(1, (*itr).guid);
        stmt->setUInt16(2, (*itr).specId);
    }

    CharacterDatabase.CommitTransaction(trans);
}

void ChallengeMgr::LoadFromDB()
{
    QueryResult result = CharacterDatabase.Query("SELECT `id`, `guildId`, `mapID`, `recordTime`, `date`, `medal` FROM `challenge`");

    if (!result)
        return;

    uint32 count = 0;

    Field* fields = NULL;
    do
    {
        fields = result->Fetch();

        Challenge *c = new Challenge;
        c->Id = fields[0].GetUInt32();
        c->guildId = fields[1].GetUInt32();
        c->mapID = fields[2].GetUInt16();
        c->recordTime = fields[3].GetUInt32();
        c->date = fields[4].GetUInt32();
        c->medal = fields[5].GetUInt8();

        m_ChallengeMap[c->Id] = c;
        CheckBestMapId(c);
        CheckBestGuildMapId(c);

        // sync guid generator
        if (c->Id >= challengeGUID)
            challengeGUID = ++c->Id;

    }while (result->NextRow());

    result = CharacterDatabase.Query("SELECT `id`, `member`, `specID` FROM `challenge_member`");
    do
    {
        fields = result->Fetch();
        ChallengeMember member;
        member.guid = fields[1].GetUInt64();
        member.specId = fields[2].GetUInt16();

        ChallengeMap::iterator itr = m_ChallengeMap.find(fields[0].GetUInt32());
        if (itr == m_ChallengeMap.end())
        {
            sLog->outError(LOG_FILTER_SQL, "Tabble challenge_member. Challenge %u for member " UI64FMTD " does not exist!", fields[0].GetUInt32(), member.guid);
            continue;
        }
        itr->second->member.insert(member);
        CheckBestMemberMapId(member.guid, itr->second);
    }while (result->NextRow());
}

void ChallengeMgr::GroupReward(Map *instance, uint32 recordTime, ChallengeMode medal)
{
    Map::PlayerList const& players = instance->GetPlayers();
    if (players.isEmpty() || medal == CHALLENGE_MEDAL_NONE)
        return;

    uint32 challengeID = GenerateChallengeID();

    Challenge *c = new Challenge;
    c->Id = challengeID;
    c->mapID = instance->GetId();
    c->recordTime = recordTime;
    c->date = time(NULL);
    c->medal = medal;

    std::map<uint32/*guild*/, uint32> guildCounter;
    for (Map::PlayerList::const_iterator i = players.begin(); i != players.end(); ++i)
        if (Player* player = i->getSource())
        {
            ChallengeMember member;
            member.guid = player->GetGUID();
            member.specId = player->GetActiveSpec();

            if (player->GetGuildId())
                guildCounter[player->GetGuildId()] += 1;

            c->member.insert(member);
            CheckBestMemberMapId(member.guid, c);
        }

    // Stupid group guild check.
    for(std::map<uint32/*guild*/, uint32>::const_iterator itr = guildCounter.begin(); itr != guildCounter.end(); ++itr)
    {
        //only full guild group could be defined
        if(itr->second == 5)
            c->guildId = itr->first;
    }

    m_ChallengeMap[c->Id] = c;
    CheckBestMapId(c);
    CheckBestGuildMapId(c);

    SaveChallengeToDB(c);
}

Challenge * ChallengeMgr::BestServerChallenge(uint16 map)
{
    ChallengeByMap::iterator itr = m_BestForMap.find(map);
    if (itr == m_BestForMap.end())
        return NULL;

    return itr->second;
}

Challenge * ChallengeMgr::BestGuildChallenge(uint32 guildId, uint16 map)
{
    if (!guildId)
        return NULL;

    GuildBestRecord::iterator itr = m_GuildBest.find(guildId);
    if (itr == m_GuildBest.end())
        return NULL;

    ChallengeByMap::iterator itr2 = itr->second.find(map);
    if (itr2 == itr->second.end())
        return NULL;

    return itr2->second;
}