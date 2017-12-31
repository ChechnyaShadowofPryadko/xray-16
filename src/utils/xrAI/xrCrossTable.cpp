////////////////////////////////////////////////////////////////////////////
//	Module 		: xrCrossTable.cpp
//	Created 	: 25.01.2003
//  Modified 	: 25.01.2003
//	Author		: Dmitriy Iassenev
//	Description : Building cross table for AI nodes
////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "xrCrossTable.h"

const pcstr GAME_LEVEL_GRAPH = "level.graph";

using FLOAT_VECTOR = xr_vector<u32>;
using FLOAT_VECTOR_VECTOR = xr_vector<FLOAT_VECTOR>;

FLOAT_VECTOR* g_tDistances;
CLevelGraph* g_tMap;
xr_vector<bool>* g_tMarks;

u32 absolute(u32 a, u32 b) { return ((a >= b) ? (a - b) : (b - a)); }
void vfRecurseUpdate(u32 dwStartNodeID, u32 percent, u32 iVertexCount)
{
    xr_vector<u32> curr_fringe, next_fringe;
    curr_fringe.reserve(g_tDistances->size());
    next_fringe.reserve(g_tDistances->size());
    g_tDistances->assign(g_tDistances->size(), u32(-1));
    curr_fringe.push_back(dwStartNodeID);
    u32 curr_dist = 0, total_count = 0;
    Logger.Progress(float(percent) / float(iVertexCount));
    for (; !curr_fringe.empty();)
    {
        for (const auto &i : curr_fringe)
        {
            (*g_tDistances)[i] = curr_dist;
            auto node = (*g_tMap).vertex(i);
            for (const auto &j : {0, 1, 2, 3})
            {
                u32 dwNexNodeID = node->link(j);
                if (!(*g_tMap).valid_vertex_id(dwNexNodeID) || (*g_tMarks)[dwNexNodeID])
                    continue;
                if ((*g_tDistances)[dwNexNodeID] > curr_dist)
                {
                    next_fringe.push_back(dwNexNodeID);
                    (*g_tMarks)[dwNexNodeID] = true;
                }
            }
        }
        for (auto &i : curr_fringe)
            (*g_tMarks)[i] = false;

        total_count += curr_fringe.size();
        curr_fringe = next_fringe;
        next_fringe.clear();
        ++curr_dist;
        Logger.Progress(float(percent) / float(iVertexCount) +
            float(total_count) / (float(iVertexCount) * float(g_tMap->header().vertex_count())));
    }
}

void vfRecurseMark(const CLevelGraph& tMap, xr_vector<bool>& tMarks, u32 dwStartNodeID)
{
    xr_vector<u32> l_stack;
    l_stack.reserve(8192);
    l_stack.push_back(dwStartNodeID);

    for (; !l_stack.empty();)
    {
        dwStartNodeID = l_stack.back();
        l_stack.resize(l_stack.size() - 1);
        auto node = tMap.vertex(dwStartNodeID);
        tMarks[dwStartNodeID] = true;
        for (const auto &j : { 0, 1, 2, 3 })
        {
            u32 dwNexNodeID = node->link(j);
            if (tMap.valid_vertex_id(dwNexNodeID) && !tMarks[dwNexNodeID])
                l_stack.push_back(dwNexNodeID);
        }
    }
}

class CCrossTableBuilder
{
public:
    CCrossTableBuilder(LPCSTR caProjectName);
};

CCrossTableBuilder::CCrossTableBuilder(LPCSTR caProjectName)
{
    FILE_NAME caFileName;
    strconcat(sizeof(caFileName), caFileName, caProjectName, GAME_LEVEL_GRAPH);

    Logger.Phase("Loading level graph");
    CGameGraph tGraph(caFileName);

    Logger.Phase("Loading AI map");
    CLevelGraph tMap(caProjectName);

    Logger.Phase("Building dynamic objects");
    FLOAT_VECTOR_VECTOR tDistances;
    int iVertexCount = tGraph.header().vertex_count();
    R_ASSERT2(iVertexCount > 0, "There are no graph points in the graph!");
    int iNodeCount = tMap.header().vertex_count();
    xr_vector<bool> tMarks;
    tMarks.assign(tMap.header().vertex_count(), false);
    {
        for (int i = 0; i < iVertexCount; i++)
            vfRecurseMark(tMap, tMarks, tGraph.vertex(i)->level_vertex_id());
        tMarks.flip();
    }

    tDistances.resize(iVertexCount);

    for (auto &i : tDistances)
    {
        i.resize(iNodeCount);
        for (auto &j : i)
            j = u32(-1);
    }

    Logger.Phase("Building cross table");
    Logger.Progress(0.f);
    for (int i = 0; i < iVertexCount; ++i)
    {
        if (i)
            for (int k = 0; k < (int)tMap.header().vertex_count(); k++)
                tDistances[i][k] = tDistances[i - 1][k];
        g_tDistances = &tDistances[i];
        g_tMap = &tMap;
        g_tMarks = &tMarks;
        vfRecurseUpdate(tGraph.vertex(i)->level_vertex_id(), i, iVertexCount);
        Logger.Progress(float(i + 1) / float(iVertexCount));
    }
    Logger.Progress(1.f);

    Logger.Phase("Saving cross table");
    CMemoryWriter tMemoryStream;
    CGameLevelCrossTable::CHeader tCrossTableHeader;

    tCrossTableHeader.dwVersion = XRAI_CURRENT_VERSION;
    tCrossTableHeader.dwNodeCount = iNodeCount;
    tCrossTableHeader.dwGraphPointCount = iVertexCount;
    tCrossTableHeader.m_level_guid = tMap.header().guid();
    tCrossTableHeader.m_game_guid = tGraph.header().guid();

    tMemoryStream.open_chunk(CROSS_TABLE_CHUNK_VERSION);
    tMemoryStream.w(&tCrossTableHeader, sizeof(tCrossTableHeader));
    tMemoryStream.close_chunk();

    tMemoryStream.open_chunk(CROSS_TABLE_CHUNK_DATA);
    {
        for (int i = 0; i < iNodeCount; i++)
        {
            CGameLevelCrossTable::CCell tCrossTableCell;
            tCrossTableCell.fDistance = flt_max;
            tCrossTableCell.tGraphIndex = u16(-1);

            for (auto &j : tDistances)
            {
                if (float(j[i]) * tMap.header().cell_size() < tCrossTableCell.fDistance)
                {
                    tCrossTableCell.fDistance = float(j[i]) * tMap.header().cell_size();
                    tCrossTableCell.tGraphIndex = GameGraph::_GRAPH_ID(std::distance(&tDistances.front(), &j));
                }
            }

            for (int j = 0; j < iVertexCount; j++)
            {
                if ((tGraph.vertex(j)->level_vertex_id() == (u32)i) && (tCrossTableCell.tGraphIndex != j))
                {
                    Msg("! Warning : graph points are too close, therefore cross table is automatically validated");
                    Msg("%d : [%f][%f][%f] %d[%f] -> %d[%f]", i, VPUSH(tGraph.vertex(j)->level_point()),
                        tCrossTableCell.tGraphIndex, tCrossTableCell.fDistance, j, tDistances[j][i]);
                    tCrossTableCell.fDistance = float(tDistances[j][i]) * tMap.header().cell_size();
                    tCrossTableCell.tGraphIndex = (GameGraph::_GRAPH_ID)j;
                }
            }

            tMemoryStream.w(&tCrossTableCell, sizeof(tCrossTableCell));
        }
    }
    tMemoryStream.close_chunk();

    strconcat(sizeof(caFileName), caFileName, caProjectName, CROSS_TABLE_NAME_RAW);
    tMemoryStream.save_to(caFileName);
}

void xrBuildCrossTable(LPCSTR caProjectName) { CCrossTableBuilder A(caProjectName); }
