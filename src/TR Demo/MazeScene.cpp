// MazeScene.cpp - Complete working Windows 95 Maze
// Corrected: internal walls visibility + simple cel/outline aesthetic
//
// Core fixes in this version:
// 1) Painter's-order fix: floor/ceiling emitted first (robust if Z ever gets toggled).
// 2) Camera facing/offset mapping fixed to match look math (rot 0 => +Z).
// 3) Wall-rise scaling removed from world.
//
// This revision focuses on MOODY FOG (no textures, no fake "lights").
//
// IMPORTANT (per your headers):
// - D3DRS_FOGVERTEXMODE is NOT supported on Xbox. Use TABLE fog only. :contentReference[oaicite:3]{index=3}
// - Fog states used here are supported: FOGENABLE/FOGTABLEMODE/FOGSTART/FOGEND/FOGDENSITY/RANGEFOGENABLE. :contentReference[oaicite:4]{index=4}

#include <xtl.h>
#include <d3d8.h>
#include <d3dx8.h>
#include <stdlib.h>
#include <string.h>

extern LPDIRECT3DDEVICE8 g_pd3dDevice;

namespace
{
    // =======================================================================
    // CONSTANTS
    // =======================================================================
    static const int   MAZE_SIZE = 10;
    static const float WALL_HEIGHT = 1.0f;
    static const float CAMERA_HEIGHT = 0.5f;

    static const int   FORCE_ALL_WALLS = 0;

    // Cel / outline tweakables
    static const int   ENABLE_OUTLINE = 1;
    static const float OUTLINE_SCALE = 1.03f;
    static const DWORD OUTLINE_COLOR = D3DCOLOR_XRGB(0, 0, 0);

    // -----------------------------------------------------------------------
    // MOODY FOG (TABLE FOG ONLY)
    // -----------------------------------------------------------------------
    static const int   ENABLE_FOG = 1;

    // Use EXP2 for a stronger "murk" feel than linear
    // (Modes are defined in your headers: NONE/EXP/EXP2/LINEAR). :contentReference[oaicite:5]{index=5}
    static const DWORD FOG_MODE = D3DFOG_EXP2;

    // Density is what drives EXP/EXP2.
    // Start around 0.14..0.22. Higher = thicker fog.
    static const float FOG_DENSITY = 0.35f;

    // Fog tint must NOT be black if you want to see it on a dark scene.
    // Dark teal/blue reads "moody" but still visible.
    static const DWORD FOG_COLOR = D3DCOLOR_XRGB(10, 18, 28);

    // These are still set (even for EXP2) because some pipelines read them.
    static const float FOG_START = 1.5f;
    static const float FOG_END = 9.0f;

    // Range fog can help distance feel more natural in corridors
    static const int   ENABLE_RANGE_FOG = 1;

    // Base colors (keep your palette punchy)
    static const DWORD FLOOR_COLOR = D3DCOLOR_XRGB(80, 95, 110);
    static const DWORD CEIL_COLOR = D3DCOLOR_XRGB(65, 75, 88);

    // =======================================================================
    // MAZE STRUCTURE
    // =======================================================================
    struct Cell
    {
        int up, down, left, right; // 1 = passage, 0 = wall
        int x, y;
    };

    static Cell g_maze[MAZE_SIZE * MAZE_SIZE];

    // =======================================================================
    // WALKER STATE
    // =======================================================================
    static int   g_cellX = 0;
    static int   g_cellY = 0;
    static int   g_direction = 2; // 0=UP, 1=RIGHT, 2=DOWN, 3=LEFT
    static float g_interpStep = 0.0f;
    static float g_posStart[3] = { 0.5f, CAMERA_HEIGHT, 0.5f };
    static float g_posEnd[3] = { 0.5f, CAMERA_HEIGHT, 0.5f };
    static float g_rotStart = 0.0f;
    static float g_rotEnd = 0.0f;

    // =======================================================================
    // RENDERING
    // =======================================================================
    struct WallVertex
    {
        float x, y, z;
        D3DCOLOR color;
    };
#define FVF_WALL (D3DFVF_XYZ | D3DFVF_DIFFUSE)

    static LPDIRECT3DVERTEXBUFFER8 g_vbWalls = NULL;
    static LPDIRECT3DINDEXBUFFER8  g_ibWalls = NULL;
    static int g_numWallVerts = 0;
    static int g_numWallIndices = 0;

    static float g_wallRiseTime = 0.0f;
    static const float WALL_RISE_DURATION = 2.0f;

    // =======================================================================
    // HELPERS
    // =======================================================================

    inline Cell* GetCell(int x, int y)
    {
        if (x < 0 || x >= MAZE_SIZE || y < 0 || y >= MAZE_SIZE) return NULL;
        return &g_maze[y * MAZE_SIZE + x];
    }

    inline int CellIsFree(int x, int y)
    {
        Cell* c = GetCell(x, y);
        return c && !c->up && !c->down && !c->left && !c->right;
    }

    // =======================================================================
    // MAZE GENERATION
    // =======================================================================

    void GenerateMazeRecursive(Cell* cell)
    {
        enum { DIR_UP = 0, DIR_DOWN = 1, DIR_LEFT = 2, DIR_RIGHT = 3 };
        int paths[4];
        int pathCount;

        while (1)
        {
            pathCount = 0;

            if (cell->y != 0 && CellIsFree(cell->x, cell->y - 1))
                paths[pathCount++] = DIR_UP;
            if (cell->y != MAZE_SIZE - 1 && CellIsFree(cell->x, cell->y + 1))
                paths[pathCount++] = DIR_DOWN;
            if (cell->x != 0 && CellIsFree(cell->x - 1, cell->y))
                paths[pathCount++] = DIR_LEFT;
            if (cell->x != MAZE_SIZE - 1 && CellIsFree(cell->x + 1, cell->y))
                paths[pathCount++] = DIR_RIGHT;

            if (!pathCount) break;

            int dir = paths[rand() % pathCount];
            Cell* newCell = NULL;

            switch (dir)
            {
            case DIR_UP:
                cell->up = 1;
                newCell = GetCell(cell->x, cell->y - 1);
                newCell->down = 1;
                break;
            case DIR_DOWN:
                cell->down = 1;
                newCell = GetCell(cell->x, cell->y + 1);
                newCell->up = 1;
                break;
            case DIR_LEFT:
                cell->left = 1;
                newCell = GetCell(cell->x - 1, cell->y);
                newCell->right = 1;
                break;
            case DIR_RIGHT:
                cell->right = 1;
                newCell = GetCell(cell->x + 1, cell->y);
                newCell->left = 1;
                break;
            }

            GenerateMazeRecursive(newCell);
        }
    }

    void GenerateMaze()
    {
        for (int y = 0; y < MAZE_SIZE; y++)
        {
            for (int x = 0; x < MAZE_SIZE; x++)
            {
                Cell* c = GetCell(x, y);
                c->x = x;
                c->y = y;
                c->up = c->down = c->left = c->right = 0;
            }
        }

        Cell* start = GetCell(rand() % MAZE_SIZE, rand() % MAZE_SIZE);
        GenerateMazeRecursive(start);
    }

    // =======================================================================
    // WALL GEOMETRY
    // =======================================================================

    void CreateWallGeometry()
    {
        auto EdgeOpenH = [&](int x, int y) -> int
            {
                if (y == 0 || y == MAZE_SIZE) return 0;
                Cell* a = GetCell(x, y - 1);
                Cell* b = GetCell(x, y);
                if (!a || !b) return 0;
                return (a->down || b->up) ? 1 : 0;
            };

        auto EdgeOpenV = [&](int x, int y) -> int
            {
                if (x == 0 || x == MAZE_SIZE) return 0;
                Cell* a = GetCell(x - 1, y);
                Cell* b = GetCell(x, y);
                if (!a || !b) return 0;
                return (a->right || b->left) ? 1 : 0;
            };

        int hWalls = 0, vWalls = 0;

        for (int y = 0; y < MAZE_SIZE + 1; y++)
        {
            for (int x = 0; x < MAZE_SIZE; x++)
            {
                if (!FORCE_ALL_WALLS && EdgeOpenH(x, y)) continue;
                hWalls++;
            }
        }

        for (int y = 0; y < MAZE_SIZE; y++)
        {
            for (int x = 0; x < MAZE_SIZE + 1; x++)
            {
                if (!FORCE_ALL_WALLS && EdgeOpenV(x, y)) continue;
                vWalls++;
            }
        }

        const int totalQuads = hWalls + vWalls + 2;

        g_numWallVerts = totalQuads * 4;
        g_numWallIndices = totalQuads * 6;

        if (g_vbWalls) { g_vbWalls->Release(); g_vbWalls = NULL; }
        if (g_ibWalls) { g_ibWalls->Release(); g_ibWalls = NULL; }

        g_pd3dDevice->CreateVertexBuffer(
            g_numWallVerts * sizeof(WallVertex),
            D3DUSAGE_WRITEONLY, FVF_WALL, D3DPOOL_MANAGED, &g_vbWalls);

        g_pd3dDevice->CreateIndexBuffer(
            g_numWallIndices * sizeof(WORD),
            D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &g_ibWalls);

        WallVertex* verts = NULL;
        g_vbWalls->Lock(0, 0, (BYTE**)&verts, 0);

        int vIdx = 0;
        const float mz = (float)MAZE_SIZE;

        // Floor + ceiling first
        verts[vIdx++] = { 0.0f, 0.0f, 0.0f, FLOOR_COLOR };
        verts[vIdx++] = { mz,   0.0f, 0.0f, FLOOR_COLOR };
        verts[vIdx++] = { mz,   0.0f, mz,   FLOOR_COLOR };
        verts[vIdx++] = { 0.0f, 0.0f, mz,   FLOOR_COLOR };

        verts[vIdx++] = { 0.0f, WALL_HEIGHT, 0.0f, CEIL_COLOR };
        verts[vIdx++] = { 0.0f, WALL_HEIGHT, mz,   CEIL_COLOR };
        verts[vIdx++] = { mz,   WALL_HEIGHT, mz,   CEIL_COLOR };
        verts[vIdx++] = { mz,   WALL_HEIGHT, 0.0f, CEIL_COLOR };

        // Horizontal walls
        for (int y = 0; y < MAZE_SIZE + 1; y++)
        {
            for (int x = 0; x < MAZE_SIZE; x++)
            {
                if (!FORCE_ALL_WALLS && EdgeOpenH(x, y)) continue;

                int colorIdx = ((x / 3) + (y / 3)) & 3;
                DWORD color =
                    (colorIdx == 0) ? D3DCOLOR_XRGB(255, 120, 120) :
                    (colorIdx == 1) ? D3DCOLOR_XRGB(120, 255, 120) :
                    (colorIdx == 2) ? D3DCOLOR_XRGB(120, 120, 255) :
                    D3DCOLOR_XRGB(255, 255, 130);

                float fx = (float)x;
                float fy = (float)y;

                verts[vIdx++] = { fx,        0.0f,        fy, color };
                verts[vIdx++] = { fx + 1.0f, 0.0f,        fy, color };
                verts[vIdx++] = { fx + 1.0f, WALL_HEIGHT, fy, color };
                verts[vIdx++] = { fx,        WALL_HEIGHT, fy, color };
            }
        }

        // Vertical walls
        for (int y = 0; y < MAZE_SIZE; y++)
        {
            for (int x = 0; x < MAZE_SIZE + 1; x++)
            {
                if (!FORCE_ALL_WALLS && EdgeOpenV(x, y)) continue;

                int colorIdx = ((x / 3) + (y / 3)) & 3;
                DWORD color =
                    (colorIdx == 0) ? D3DCOLOR_XRGB(255, 120, 120) :
                    (colorIdx == 1) ? D3DCOLOR_XRGB(120, 255, 120) :
                    (colorIdx == 2) ? D3DCOLOR_XRGB(120, 120, 255) :
                    D3DCOLOR_XRGB(255, 255, 130);

                float fx = (float)x;
                float fy = (float)y;

                verts[vIdx++] = { fx, 0.0f,        fy,        color };
                verts[vIdx++] = { fx, 0.0f,        fy + 1.0f,  color };
                verts[vIdx++] = { fx, WALL_HEIGHT, fy + 1.0f,  color };
                verts[vIdx++] = { fx, WALL_HEIGHT, fy,        color };
            }
        }

        g_vbWalls->Unlock();

        WORD* indices = NULL;
        g_ibWalls->Lock(0, 0, (BYTE**)&indices, 0);

        for (int i = 0; i < totalQuads; i++)
        {
            int base = i * 4;
            int iIdx = i * 6;

            indices[iIdx + 0] = (WORD)(base + 0);
            indices[iIdx + 1] = (WORD)(base + 1);
            indices[iIdx + 2] = (WORD)(base + 2);
            indices[iIdx + 3] = (WORD)(base + 0);
            indices[iIdx + 4] = (WORD)(base + 2);
            indices[iIdx + 5] = (WORD)(base + 3);
        }

        g_ibWalls->Unlock();
    }

    // =======================================================================
    // WALKER
    // =======================================================================

    void GetGlobalPosition(int cellX, int cellY, int direction, float pos[3])
    {
        pos[0] = (float)cellX + 0.5f;
        pos[1] = CAMERA_HEIGHT;
        pos[2] = (float)cellY + 0.5f;

        switch (direction)
        {
        case 0: pos[2] -= 0.25f; break; // UP (-Z)
        case 1: pos[0] += 0.25f; break; // RIGHT (+X)
        case 2: pos[2] += 0.25f; break; // DOWN (+Z)
        case 3: pos[0] -= 0.25f; break; // LEFT (-X)
        }
    }

    int CellPassageInDirection(Cell* cell, int dir)
    {
        switch (dir)
        {
        case 0: return cell->up;
        case 1: return cell->right;
        case 2: return cell->down;
        case 3: return cell->left;
        }
        return 0;
    }

    void WalkStraight()
    {
        switch (g_direction)
        {
        case 0: g_cellY--; break;
        case 1: g_cellX++; break;
        case 2: g_cellY++; break;
        case 3: g_cellX--; break;
        }
        GetGlobalPosition(g_cellX, g_cellY, g_direction, g_posEnd);
    }

    void WalkRight() { g_direction = (g_direction + 1) % 4; WalkStraight(); g_rotEnd += 90.0f; }
    void WalkLeft() { g_direction = (g_direction + 3) % 4; WalkStraight(); g_rotEnd -= 90.0f; }
    void WalkTurn() { g_direction = (g_direction + 2) % 4; WalkStraight(); g_rotEnd += 180.0f; }

    void CreateNewMove()
    {
        Cell* cur = GetCell(g_cellX, g_cellY);
        if (!cur) return;

        int right = (g_direction + 1) % 4;
        int left = (g_direction + 3) % 4;

        if (CellPassageInDirection(cur, right))
            WalkRight();
        else if (CellPassageInDirection(cur, g_direction))
            WalkStraight();
        else if (CellPassageInDirection(cur, left))
            WalkLeft();
        else
            WalkTurn();
    }

    void PickStartNotFacingWall()
    {
        for (int tries = 0; tries < 128; tries++)
        {
            int x = rand() % MAZE_SIZE;
            int y = rand() % MAZE_SIZE;
            Cell* c = GetCell(x, y);
            if (!c) continue;

            int dirs[4] = { 0,1,2,3 };
            for (int i = 0; i < 4; i++)
            {
                int j = rand() & 3;
                int t = dirs[i]; dirs[i] = dirs[j]; dirs[j] = t;
            }

            for (int k = 0; k < 4; k++)
            {
                int d = dirs[k];
                if (CellPassageInDirection(c, d))
                {
                    g_cellX = x;
                    g_cellY = y;
                    g_direction = d;

                    float rot =
                        (d == 2) ? 0.0f :
                        (d == 1) ? 90.0f :
                        (d == 0) ? 180.0f :
                        -90.0f;

                    g_rotStart = g_rotEnd = rot;

                    GetGlobalPosition(g_cellX, g_cellY, g_direction, g_posStart);
                    memcpy(g_posEnd, g_posStart, sizeof(g_posStart));
                    return;
                }
            }
        }

        g_cellX = 0;
        g_cellY = 0;
        g_direction = 2;
        g_rotStart = g_rotEnd = 0.0f;
        GetGlobalPosition(g_cellX, g_cellY, g_direction, g_posStart);
        memcpy(g_posEnd, g_posStart, sizeof(g_posStart));
    }

    // =======================================================================
    // OUTLINE PASS
    // =======================================================================

    void SetupOutlineFixedFunction()
    {
        g_pd3dDevice->SetTexture(0, NULL);

        g_pd3dDevice->SetRenderState(D3DRS_TEXTUREFACTOR, OUTLINE_COLOR);

        g_pd3dDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        g_pd3dDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TFACTOR);
        g_pd3dDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

        g_pd3dDevice->SetRenderState(D3DRS_COLORVERTEX, FALSE);
        g_pd3dDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_CW);

        g_pd3dDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
        g_pd3dDevice->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        g_pd3dDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        g_pd3dDevice->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    }

    void RestoreColorVertex()
    {
        g_pd3dDevice->SetRenderState(D3DRS_COLORVERTEX, TRUE);
        g_pd3dDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_DISABLE);
        g_pd3dDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    }

} // namespace

// =======================================================================
// PUBLIC API
// =======================================================================

void MazeScene_Init()
{
    GenerateMaze();
    CreateWallGeometry();

    g_interpStep = 0.0f;

    g_wallRiseTime = WALL_RISE_DURATION; // skip rise

    PickStartNotFacingWall();
}

void MazeScene_Shutdown()
{
    if (g_vbWalls) g_vbWalls->Release();
    if (g_ibWalls) g_ibWalls->Release();
    g_vbWalls = NULL;
    g_ibWalls = NULL;
}

void MazeScene_Update()
{
    if (g_wallRiseTime < WALL_RISE_DURATION)
    {
        g_wallRiseTime += 1.0f / 60.0f;
        return;
    }

    g_interpStep += 1.0f / 60.0f;

    if (g_interpStep >= 1.0f)
    {
        g_interpStep = 0.0f;
        memcpy(g_posStart, g_posEnd, sizeof(g_posStart));
        g_rotStart = g_rotEnd;

        CreateNewMove();
    }
}

void MazeScene_Render()
{
    // hard reset
    g_pd3dDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_pd3dDevice->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);

    g_pd3dDevice->SetRenderState(D3DRS_ZENABLE, TRUE);
    g_pd3dDevice->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    g_pd3dDevice->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);

    g_pd3dDevice->SetTexture(0, NULL);
    g_pd3dDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_DISABLE);
    g_pd3dDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

    // interpolate
    float t = g_interpStep;
    float camPos[3];
    camPos[0] = g_posStart[0] + (g_posEnd[0] - g_posStart[0]) * t;
    camPos[1] = g_posStart[1] + (g_posEnd[1] - g_posStart[1]) * t;
    camPos[2] = g_posStart[2] + (g_posEnd[2] - g_posStart[2]) * t;

    float camRot = g_rotStart + (g_rotEnd - g_rotStart) * t;

    float radians = D3DXToRadian(camRot);
    float lookX = camPos[0] + sinf(radians);
    float lookZ = camPos[2] + cosf(radians);

    // view
    D3DXMATRIX matView;
    D3DXVECTOR3 eye(camPos[0], camPos[1], camPos[2]);
    D3DXVECTOR3 at(lookX, camPos[1], lookZ);
    D3DXVECTOR3 up(0.0f, 1.0f, 0.0f);
    D3DXMatrixLookAtLH(&matView, &eye, &at, &up);
    g_pd3dDevice->SetTransform(D3DTS_VIEW, &matView);

    // proj
    D3DXMATRIX matProj;
    D3DXMatrixPerspectiveFovLH(&matProj, D3DXToRadian(90.0f), 640.0f / 480.0f, 0.1f, 50.0f);
    g_pd3dDevice->SetTransform(D3DTS_PROJECTION, &matProj);

    // cel-ish
    g_pd3dDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_pd3dDevice->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_FLAT);
    g_pd3dDevice->SetRenderState(D3DRS_COLORVERTEX, TRUE);
    g_pd3dDevice->SetRenderState(D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_COLOR1);
    g_pd3dDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);

    // -------------------------------------------------------------------
    // MOODY FOG (TABLE FOG)
    // Render states exist in your headers. :contentReference[oaicite:6]{index=6}
    // -------------------------------------------------------------------
    if (ENABLE_FOG)
    {
        g_pd3dDevice->SetRenderState(D3DRS_FOGENABLE, TRUE);
        g_pd3dDevice->SetRenderState(D3DRS_FOGCOLOR, FOG_COLOR);

        // EXP2 fog (stronger mood). Mode enum is defined in headers. :contentReference[oaicite:7]{index=7}
        g_pd3dDevice->SetRenderState(D3DRS_FOGTABLEMODE, FOG_MODE);

        // Density drives EXP/EXP2
        float d = FOG_DENSITY;
        g_pd3dDevice->SetRenderState(D3DRS_FOGDENSITY, *(DWORD*)(&d));

        // Also set start/end (harmless, and helps if the pipeline leans on them)
        float s = FOG_START;
        float e = FOG_END;
        g_pd3dDevice->SetRenderState(D3DRS_FOGSTART, *(DWORD*)(&s));
        g_pd3dDevice->SetRenderState(D3DRS_FOGEND, *(DWORD*)(&e));

        g_pd3dDevice->SetRenderState(D3DRS_RANGEFOGENABLE, (ENABLE_RANGE_FOG ? TRUE : FALSE));
    }
    else
    {
        g_pd3dDevice->SetRenderState(D3DRS_FOGENABLE, FALSE);
    }

    // world
    D3DXMATRIX matWorld;
    D3DXMatrixIdentity(&matWorld);

    // bind
    g_pd3dDevice->SetVertexShader(FVF_WALL);
    g_pd3dDevice->SetStreamSource(0, g_vbWalls, sizeof(WallVertex));
    g_pd3dDevice->SetIndices(g_ibWalls, 0);

    // outline
    if (ENABLE_OUTLINE)
    {
        const float mz = (float)MAZE_SIZE;
        const float cx = mz * 0.5f;
        const float cz = mz * 0.5f;

        D3DXMATRIX T1, S, T2;
        D3DXMatrixTranslation(&T1, -cx, 0.0f, -cz);
        D3DXMatrixScaling(&S, OUTLINE_SCALE, OUTLINE_SCALE, OUTLINE_SCALE);
        D3DXMatrixTranslation(&T2, cx, 0.0f, cz);

        D3DXMATRIX outlineWorld = T1 * S * T2 * matWorld;

        SetupOutlineFixedFunction();
        g_pd3dDevice->SetTransform(D3DTS_WORLD, &outlineWorld);
        g_pd3dDevice->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, g_numWallVerts, 0, g_numWallIndices / 3);

        RestoreColorVertex();
        g_pd3dDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        g_pd3dDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
        g_pd3dDevice->SetRenderState(D3DRS_ZENABLE, TRUE);
        g_pd3dDevice->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        g_pd3dDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        g_pd3dDevice->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    }

    // main
    g_pd3dDevice->SetTransform(D3DTS_WORLD, &matWorld);
    g_pd3dDevice->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, g_numWallVerts, 0, g_numWallIndices / 3);

    // restore
    g_pd3dDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
    g_pd3dDevice->SetRenderState(D3DRS_FOGENABLE, FALSE);
    g_pd3dDevice->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
}
