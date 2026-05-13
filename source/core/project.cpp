// =============================================================================
// project.cpp — Parses Scratch 3.0 project.json using jsmn
// Matches the optimized project.h:
//   - ScratchBlock uses fixed char arrays (no std::string for id/nextId/etc.)
//   - ScratchBlock inputs/fields use flat arrays (no std::map)
//   - ScratchVariable uses fixed char arrays for id and name
//   - opcodeFromStr takes (const char*, int)
//   - parseBlockInputs / parseBlockFields take ScratchBlock& (not map refs)
// =============================================================================
#ifndef JSMN_STATIC
#define JSMN_STATIC
#endif
#include "jsmn.h"
#include "project.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>

// -----------------------------------------------------------------------
// jsmn helpers
// -----------------------------------------------------------------------
static bool jsmnEq(const char* json, jsmntok_t* tok, const char* s) {
    return (tok->type == JSMN_STRING
        && (int)strlen(s) == tok->end - tok->start
        && strncmp(json + tok->start, s, tok->end - tok->start) == 0);
}

static std::string jsmnStr(const char* json, jsmntok_t* tok) {
    if (!tok || tok->type == JSMN_UNDEFINED) return "";
    return std::string(json + tok->start, tok->end - tok->start);
}

// Copy jsmn token string into a fixed-size buffer (null-terminates, truncates)
static void jsmnStrCopy(const char* json, jsmntok_t* tok, char* buf, int maxLen) {
    if (!tok || tok->type == JSMN_UNDEFINED) { buf[0] = '\0'; return; }
    int len = tok->end - tok->start;
    if (len >= maxLen) len = maxLen - 1;
    strncpy(buf, json + tok->start, len);
    buf[len] = '\0';
}

static double jsmnNum(const char* json, jsmntok_t* tok) {
    if (!tok) return 0.0;
    char buf[64];
    int len = tok->end - tok->start;
    if (len >= 63) len = 63;
    strncpy(buf, json + tok->start, len);
    buf[len] = '\0';
    return atof(buf);
}

static bool jsmnBool(const char* json, jsmntok_t* tok) {
    return (tok->end - tok->start == 4 &&
            strncmp(json + tok->start, "true", 4) == 0);
}

// -----------------------------------------------------------------------
// Load project from extracted directory
// -----------------------------------------------------------------------
bool ScratchProject::load(const char* dir) {
    extractDir = dir;

    char jsonPath[512];
    snprintf(jsonPath, sizeof(jsonPath), "%s/project.json", dir);

    FILE* f = fopen(jsonPath, "rb");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    char* json = (char*)malloc(sz + 1);
    if (!json) { fclose(f); return false; }
    fread(json, 1, sz, f);
    json[sz] = '\0';
    fclose(f);

    bool ok = parseJson(json, (size_t)sz);
    free(json);
    return ok;
}

// -----------------------------------------------------------------------
// Skip over a single JSON value (any type, any depth)
// -----------------------------------------------------------------------
void ScratchProject::skipValue(jsmntok_t* toks, int& i, int numToks) {
    if (i >= numToks) return;
    int type = toks[i].type;
    int size = toks[i].size;
    i++;
    if (type == JSMN_OBJECT) {
        for (int k = 0; k < size; k++) {
            skipValue(toks, i, numToks); // key
            skipValue(toks, i, numToks); // value
        }
    } else if (type == JSMN_ARRAY) {
        for (int k = 0; k < size; k++) {
            skipValue(toks, i, numToks);
        }
    }
}

// -----------------------------------------------------------------------
// Main JSON parser
// -----------------------------------------------------------------------
bool ScratchProject::parseJson(const char* json, size_t len) {
    jsmn_parser p;
    jsmn_init(&p);

    int numToks = jsmn_parse(&p, json, len, nullptr, 0);
    if (numToks < 1) return false;

    jsmntok_t* toks = (jsmntok_t*)malloc((size_t)numToks * sizeof(jsmntok_t));
    if (!toks) return false;

    jsmn_init(&p);
    if (jsmn_parse(&p, json, len, toks, (unsigned)numToks) < 1) {
        free(toks);
        return false;
    }

    if (toks[0].type != JSMN_OBJECT) { free(toks); return false; }

    int i = 1;
    int rootKeys = toks[0].size;

    for (int k = 0; k < rootKeys && i < numToks; k++) {
        if (i >= numToks) break;
        if (jsmnEq(json, &toks[i], "meta")) {
            i++;
            if (i < numToks && toks[i].type == JSMN_OBJECT) {
                int metaKeys = toks[i].size;
                i++;
                for (int mk = 0; mk < metaKeys && i < numToks; mk++) {
                    std::string key = jsmnStr(json, &toks[i]); i++;
                    if (i >= numToks) break;
                    if      (key == "semver") meta.semver = jsmnStr(json, &toks[i]);
                    else if (key == "vm")     meta.vm     = jsmnStr(json, &toks[i]);
                    else if (key == "agent")  meta.agent  = jsmnStr(json, &toks[i]);
                    i++;
                }
            } else {
                skipValue(toks, i, numToks);
            }
        } else if (jsmnEq(json, &toks[i], "targets")) {
            i++;
            if (i < numToks && toks[i].type == JSMN_ARRAY) {
                int numTargets = toks[i].size;
                i++;
                targets.reserve((size_t)numTargets < (size_t)MAX_SPRITES
                                 ? numTargets : MAX_SPRITES);
                for (int t = 0; t < numTargets && i < numToks; t++) {
                    if ((int)targets.size() >= MAX_SPRITES) {
                        skipValue(toks, i, numToks);
                        continue;
                    }
                    ScratchSprite sprite;
                    sprite.visible = true;
                    sprite.x = sprite.y = 0;
                    sprite.size = 100;
                    sprite.direction = 90;
                    sprite.currentCostume = 0;
                    sprite.isStage = false;
                    sprite.isClone = false;
                    sprite.oamId = -1;
                    sprite.cloneParentIndex = -1;
                    sprite.layerOrder = 0;
                    parseTarget(json, toks, i, numToks, sprite);
                    sprite.buildBlockIndex();
                    targets.push_back(std::move(sprite));
                }
            } else {
                skipValue(toks, i, numToks);
            }
        } else {
            i++; // skip key
            skipValue(toks, i, numToks); // skip value
        }
    }

    free(toks);
    return !targets.empty();
}

// -----------------------------------------------------------------------
// Parse a single target object
// -----------------------------------------------------------------------
void ScratchProject::parseTarget(const char* json, jsmntok_t* toks,
                                  int& i, int numToks, ScratchSprite& sprite) {
    if (i >= numToks || toks[i].type != JSMN_OBJECT) {
        skipValue(toks, i, numToks);
        return;
    }
    int numKeys = toks[i].size;
    i++;

    for (int k = 0; k < numKeys && i < numToks; k++) {
        std::string key = jsmnStr(json, &toks[i]); i++;
        if (i >= numToks) break;

        if      (key == "isStage")        { sprite.isStage = jsmnBool(json, &toks[i]); i++; }
        else if (key == "name")           { sprite.name    = jsmnStr(json, &toks[i]);  i++; }
        else if (key == "x")              { sprite.x       = jsmnNum(json, &toks[i]);  i++; }
        else if (key == "y")              { sprite.y       = jsmnNum(json, &toks[i]);  i++; }
        else if (key == "size")           { sprite.size    = jsmnNum(json, &toks[i]);  i++; }
        else if (key == "direction")      { sprite.direction = (int)jsmnNum(json, &toks[i]); i++; }
        else if (key == "visible")        { sprite.visible = jsmnBool(json, &toks[i]); i++; }
        else if (key == "currentCostume") { sprite.currentCostume = (int)jsmnNum(json, &toks[i]); i++; }
        else if (key == "rotationStyle")  { sprite.rotationStyle = jsmnStr(json, &toks[i]); i++; }
        else if (key == "layerOrder")     { sprite.layerOrder = (int)jsmnNum(json, &toks[i]); i++; }
        else if (key == "costumes")   { parseCostumes(json, toks, i, numToks, sprite.costumes); }
        else if (key == "sounds")     { parseSounds(json, toks, i, numToks, sprite.sounds); }
        else if (key == "blocks")     { parseBlocks(json, toks, i, numToks, sprite.blocks); }
        else if (key == "variables")  { parseVariables(json, toks, i, numToks, sprite.variables); }
        else                          { skipValue(toks, i, numToks); }
    }
}

// -----------------------------------------------------------------------
// Parse costumes array
// -----------------------------------------------------------------------
void ScratchProject::parseCostumes(const char* json, jsmntok_t* toks,
                                    int& i, int numToks,
                                    std::vector<ScratchCostume>& costumes) {
    if (i >= numToks || toks[i].type != JSMN_ARRAY) {
        skipValue(toks, i, numToks); return;
    }
    int n = toks[i].size; i++;
    costumes.reserve(n);
    for (int c = 0; c < n && i < numToks; c++) {
        ScratchCostume costume{};
        costume.bitmapResolution = 1;
        costume.gfxPtr = nullptr;
        costume.palSlot = -1;
        costume.width = costume.height = 0;
        costume.isBackdrop = false;
        if (i >= numToks || toks[i].type != JSMN_OBJECT) {
            skipValue(toks, i, numToks); continue;
        }
        int nk = toks[i].size; i++;
        for (int k = 0; k < nk && i < numToks; k++) {
            std::string key = jsmnStr(json, &toks[i]); i++;
            if (i >= numToks) break;
            if      (key == "name")             { costume.name = jsmnStr(json, &toks[i]); i++; }
            else if (key == "assetId")          { costume.assetId = jsmnStr(json, &toks[i]); i++; }
            else if (key == "dataFormat")       { costume.dataFormat = jsmnStr(json, &toks[i]); i++; }
            else if (key == "bitmapResolution") { costume.bitmapResolution = (int)jsmnNum(json, &toks[i]); i++; }
            else if (key == "rotationCenterX")  { costume.rotationCenterX = jsmnNum(json, &toks[i]); i++; }
            else if (key == "rotationCenterY")  { costume.rotationCenterY = jsmnNum(json, &toks[i]); i++; }
            else                                { skipValue(toks, i, numToks); }
        }
        costumes.push_back(std::move(costume));
    }
}

// -----------------------------------------------------------------------
// Parse sounds array
// -----------------------------------------------------------------------
void ScratchProject::parseSounds(const char* json, jsmntok_t* toks,
                                  int& i, int numToks,
                                  std::vector<ScratchSound>& sounds) {
    if (i >= numToks || toks[i].type != JSMN_ARRAY) {
        skipValue(toks, i, numToks); return;
    }
    int n = toks[i].size; i++;
    sounds.reserve(n);
    for (int s = 0; s < n && i < numToks; s++) {
        ScratchSound sound{};
        sound.rate = 44100;
        sound.loaded = false;
        sound.isStreamed = false;
        sound.pcmData = nullptr;
        sound.pcmSize = 0;
        sound.mmSoundId = 0;
        sound.sampleCount = 0;
        if (i >= numToks || toks[i].type != JSMN_OBJECT) {
            skipValue(toks, i, numToks); continue;
        }
        int nk = toks[i].size; i++;
        for (int k = 0; k < nk && i < numToks; k++) {
            std::string key = jsmnStr(json, &toks[i]); i++;
            if (i >= numToks) break;
            if      (key == "name")        { sound.name = jsmnStr(json, &toks[i]); i++; }
            else if (key == "assetId")     { sound.assetId = jsmnStr(json, &toks[i]); i++; }
            else if (key == "dataFormat")  { sound.dataFormat = jsmnStr(json, &toks[i]); i++; }
            else if (key == "rate")        { sound.rate = jsmnNum(json, &toks[i]); i++; }
            else if (key == "sampleCount") { sound.sampleCount = (int)jsmnNum(json, &toks[i]); i++; }
            else                           { skipValue(toks, i, numToks); }
        }
        sounds.push_back(std::move(sound));
    }
}

// -----------------------------------------------------------------------
// Parse blocks object  (id -> ScratchBlock)
// Uses flat arrays for inputs/fields as per optimized project.h
// -----------------------------------------------------------------------
void ScratchProject::parseBlocks(const char* json, jsmntok_t* toks,
                                  int& i, int numToks,
                                  std::vector<ScratchBlock>& blocks) {
    if (i >= numToks || toks[i].type != JSMN_OBJECT) {
        skipValue(toks, i, numToks); return;
    }
    int n = toks[i].size; i++;
    blocks.reserve(n < MAX_BLOCKS ? n : MAX_BLOCKS);
    for (int b = 0; b < n && i < numToks; b++) {
        // Parse block ID into fixed buffer
        char blockId[MAX_BLOCK_ID + 1];
        jsmnStrCopy(json, &toks[i], blockId, MAX_BLOCK_ID + 1);
        i++;

        if (i >= numToks || toks[i].type != JSMN_OBJECT) {
            skipValue(toks, i, numToks); continue;
        }

        ScratchBlock block;
        strncpy(block.id, blockId, MAX_BLOCK_ID);
        block.id[MAX_BLOCK_ID] = '\0';
        block.opcode = BlockOpcode::UNKNOWN;
        block.parentId[0] = '\0';
        block.nextId[0] = '\0';
        block.topLevel = false;
        block.shadow   = false;
        block.numInputs = 0;
        block.numFields = 0;

        int nk = toks[i].size; i++;
        for (int k = 0; k < nk && i < numToks; k++) {
            std::string key = jsmnStr(json, &toks[i]); i++;
            if (i >= numToks) break;

            if (key == "opcode") {
                // Resolve opcode from the raw string
                int slen = toks[i].end - toks[i].start;
                const char* sptr = json + toks[i].start;
                block.opcode = opcodeFromStr(sptr, slen);
                i++;
            } else if (key == "next") {
                if (toks[i].type == JSMN_STRING)
                    jsmnStrCopy(json, &toks[i], block.nextId, MAX_BLOCK_ID + 1);
                else
                    block.nextId[0] = '\0';
                i++;
            } else if (key == "parent") {
                if (toks[i].type == JSMN_STRING)
                    jsmnStrCopy(json, &toks[i], block.parentId, MAX_BLOCK_ID + 1);
                else
                    block.parentId[0] = '\0';
                i++;
            } else if (key == "topLevel") {
                block.topLevel = jsmnBool(json, &toks[i]); i++;
            } else if (key == "shadow") {
                block.shadow = jsmnBool(json, &toks[i]); i++;
            } else if (key == "inputs") {
                parseBlockInputs(json, toks, i, numToks, block);
            } else if (key == "fields") {
                parseBlockFields(json, toks, i, numToks, block);
            } else {
                skipValue(toks, i, numToks);
            }
        }
        if ((int)blocks.size() < MAX_BLOCKS)
            blocks.push_back(std::move(block));
    }
}

// -----------------------------------------------------------------------
// Parse block inputs object — stores into block.inputs[] flat array
// -----------------------------------------------------------------------
void ScratchProject::parseBlockInputs(const char* json, jsmntok_t* toks,
                                       int& i, int numToks,
                                       ScratchBlock& block) {
    if (i >= numToks || toks[i].type != JSMN_OBJECT) {
        skipValue(toks, i, numToks); return;
    }
    int n = toks[i].size; i++;
    for (int k = 0; k < n && i < numToks; k++) {
        // Input slot name (e.g. "NUM1", "SUBSTACK", "CONDITION")
        char inputName[16];
        jsmnStrCopy(json, &toks[i], inputName, 16);
        i++;

        ScratchInput inp{};
        strncpy(inp.key, inputName, 15);
        inp.key[15] = '\0';
        inp.isShadow = false;
        inp.valueType = 0;
        inp.numValue = 0.0;
        inp.blockId[0] = '\0';

        // Input value: array [shadowType, value_or_blockId, ...]
        if (i < numToks && toks[i].type == JSMN_ARRAY) {
            int arrSz = toks[i].size; i++;
            // element 0: shadow indicator (int)
            if (arrSz > 0 && i < numToks) {
                inp.isShadow = (toks[i].type == JSMN_PRIMITIVE &&
                                json[toks[i].start] == '1');
                i++;
            }
            // element 1: block ID string or literal value array or null
            if (arrSz > 1 && i < numToks) {
                if (toks[i].type == JSMN_STRING) {
                    jsmnStrCopy(json, &toks[i], inp.blockId, MAX_BLOCK_ID + 1);
                    i++;
                } else if (toks[i].type == JSMN_ARRAY) {
                    int valSz = toks[i].size; i++;
                    if (valSz >= 1 && i < numToks) {
                        inp.valueType = (int)jsmnNum(json, &toks[i]); i++;
                    }
                    if (valSz >= 2 && i < numToks) {
                        inp.strValue = jsmnStr(json, &toks[i]);
                        inp.numValue = atof(inp.strValue.c_str());
                        i++;
                    }
                    for (int v = 2; v < valSz && i < numToks; v++)
                        skipValue(toks, i, numToks);
                } else if (toks[i].type == JSMN_PRIMITIVE) {
                    // null — no block reference
                    i++;
                } else {
                    skipValue(toks, i, numToks);
                }
                // skip any extra array elements beyond index 1
                for (int v = 2; v < arrSz && i < numToks; v++)
                    skipValue(toks, i, numToks);
            }
        } else {
            skipValue(toks, i, numToks);
        }

        // Add to flat inputs array if space available
        if (block.numInputs < MAX_INPUTS) {
            block.inputs[block.numInputs++] = inp;
        }
    }
}

// -----------------------------------------------------------------------
// Parse block fields object — stores into block.fields[] flat array
// -----------------------------------------------------------------------
void ScratchProject::parseBlockFields(const char* json, jsmntok_t* toks,
                                       int& i, int numToks,
                                       ScratchBlock& block) {
    if (i >= numToks || toks[i].type != JSMN_OBJECT) {
        skipValue(toks, i, numToks); return;
    }
    int n = toks[i].size; i++;
    for (int k = 0; k < n && i < numToks; k++) {
        char fieldName[16];
        jsmnStrCopy(json, &toks[i], fieldName, 16);
        i++;

        char fieldVal[MAX_VAR_NAME + 1];
        fieldVal[0] = '\0';

        // Field value is an array: [value, id_or_null]
        if (i < numToks && toks[i].type == JSMN_ARRAY) {
            int sz = toks[i].size; i++;
            if (sz > 0 && i < numToks) {
                jsmnStrCopy(json, &toks[i], fieldVal, MAX_VAR_NAME + 1);
                i++;
            }
            for (int v = 1; v < sz && i < numToks; v++)
                skipValue(toks, i, numToks);
        } else {
            skipValue(toks, i, numToks);
        }

        // Add to flat fields array if space available
        if (block.numFields < MAX_FIELDS) {
            ScratchBlock::Field& f = block.fields[block.numFields++];
            strncpy(f.key, fieldName, 15);
            f.key[15] = '\0';
            strncpy(f.value, fieldVal, MAX_VAR_NAME);
            f.value[MAX_VAR_NAME] = '\0';
        }
    }
}

// -----------------------------------------------------------------------
// Parse variables object
// -----------------------------------------------------------------------
void ScratchProject::parseVariables(const char* json, jsmntok_t* toks,
                                     int& i, int numToks,
                                     std::vector<ScratchVariable>& vars) {
    if (i >= numToks || toks[i].type != JSMN_OBJECT) {
        skipValue(toks, i, numToks); return;
    }
    int n = toks[i].size; i++;
    for (int k = 0; k < n && i < numToks; k++) {
        // Variable ID into fixed buffer
        ScratchVariable var{};
        var.isCloud = false;
        var.visible = false;

        jsmnStrCopy(json, &toks[i], var.id, MAX_BLOCK_ID + 1);
        i++;

        // Variable value is an array: [name, value, optional:isCloud]
        if (i < numToks && toks[i].type == JSMN_ARRAY) {
            int sz = toks[i].size; i++;
            if (sz > 0 && i < numToks) {
                jsmnStrCopy(json, &toks[i], var.name, MAX_VAR_NAME + 1);
                i++;
            }
            if (sz > 1 && i < numToks) {
                var.value = jsmnStr(json, &toks[i]);
                i++;
            }
            for (int v = 2; v < sz && i < numToks; v++)
                skipValue(toks, i, numToks);
        } else {
            skipValue(toks, i, numToks);
        }
        vars.push_back(var);
    }
}

// -----------------------------------------------------------------------
// ScratchSprite::buildBlockIndex
// Build a sorted index for O(log n) block lookup by ID.
// Called once after all blocks are parsed.
// -----------------------------------------------------------------------
void ScratchSprite::buildBlockIndex() {
    blockIndex.clear();
    blockIndex.reserve(blocks.size());
    for (int idx = 0; idx < (int)blocks.size(); idx++) {
        IdxEntry e;
        strncpy(e.id, blocks[idx].id, MAX_BLOCK_ID);
        e.id[MAX_BLOCK_ID] = '\0';
        e.blockIdx = (uint16_t)idx;
        blockIndex.push_back(e);
    }
    // Sort by ID string for binary search
    std::sort(blockIndex.begin(), blockIndex.end(),
              [](const IdxEntry& a, const IdxEntry& b) {
                  return __builtin_strcmp(a.id, b.id) < 0;
              });
}

// -----------------------------------------------------------------------
// Find a sprite by name
// -----------------------------------------------------------------------
ScratchSprite* ScratchProject::findSprite(const std::string& name) {
    for (auto& s : targets) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

// -----------------------------------------------------------------------
// Opcode string -> enum
// Takes (const char* s, int len) to avoid std::string construction at parse time
// -----------------------------------------------------------------------
BlockOpcode ScratchProject::opcodeFromStr(const char* s, int len) {
    // Use a macro to compare without constructing std::string
#define OP(str, code) \
    if (len == (int)(sizeof(str)-1) && strncmp(s, str, len) == 0) return BlockOpcode::code;

    OP("event_whenflagclicked",       EVENT_WHENFLAGCLICKED)
    OP("event_whenkeypressed",        EVENT_WHENKEYPRESSED)
    OP("event_whenthisspriteclicked", EVENT_WHENTHISSPRITECLICKED)
    OP("event_whenbroadcastreceived", EVENT_WHENBROADCASTRECEIVED)
    OP("motion_movesteps",            MOTION_MOVESTEPS)
    OP("motion_turnright",            MOTION_TURNRIGHT)
    OP("motion_turnleft",             MOTION_TURNLEFT)
    OP("motion_gotoxy",               MOTION_GOTOXY)
    OP("motion_glideto",              MOTION_GLIDETO)
    OP("motion_setx",                 MOTION_SETX)
    OP("motion_sety",                 MOTION_SETY)
    OP("motion_changexby",            MOTION_CHANGEXBY)
    OP("motion_changeyby",            MOTION_CHANGEYBY)
    OP("motion_ifonedgebounce",       MOTION_IFONEDGEBOUNCE)
    OP("motion_setrotationstyle",     MOTION_SETROTATIONSTYLE)
    OP("motion_xposition",            MOTION_XPOSITION)
    OP("motion_yposition",            MOTION_YPOSITION)
    OP("motion_direction",            MOTION_DIRECTION)
    OP("looks_sayforsecs",            LOOKS_SAYFORSECS)
    OP("looks_say",                   LOOKS_SAY)
    OP("looks_switchcostumeto",       LOOKS_SWITCHCOSTUMETO)
    OP("looks_nextcostume",           LOOKS_NEXTCOSTUME)
    OP("looks_switchbackdropto",      LOOKS_SWITCHBACKDROPTO)
    OP("looks_changesizeby",          LOOKS_CHANGESIZEBY)
    OP("looks_setsizeto",             LOOKS_SETSIZETO)
    OP("looks_show",                  LOOKS_SHOW)
    OP("looks_hide",                  LOOKS_HIDE)
    OP("looks_seteffectto",           LOOKS_SETEFFECTTO)
    OP("looks_changeeffectby",        LOOKS_CHANGEEFFECTBY)
    OP("sound_playuntildone",         SOUND_PLAYUNTILDONE)
    OP("sound_play",                  SOUND_PLAY)
    OP("sound_stopallsounds",         SOUND_STOPALLSOUNDS)
    OP("sound_changevolumeby",        SOUND_CHANGEVOLUMEBY)
    OP("sound_setvolumeto",           SOUND_SETVOLUMETO)
    OP("control_wait",                CONTROL_WAIT)
    OP("control_repeat",              CONTROL_REPEAT)
    OP("control_forever",             CONTROL_FOREVER)
    OP("control_if",                  CONTROL_IF)
    OP("control_if_else",             CONTROL_IF_ELSE)
    OP("control_wait_until",          CONTROL_WAIT_UNTIL)
    OP("control_repeat_until",        CONTROL_REPEAT_UNTIL)
    OP("control_stop",                CONTROL_STOP)
    OP("control_start_as_clone",      CONTROL_START_AS_CLONE)
    OP("control_create_clone_of",     CONTROL_CREATE_CLONE_OF)
    OP("control_delete_this_clone",   CONTROL_DELETE_THIS_CLONE)
    OP("sensing_touchingobject",      SENSING_TOUCHINGOBJECT)
    OP("sensing_keypressed",          SENSING_KEYPRESSED)
    OP("sensing_mousedown",           SENSING_MOUSEDOWN)
    OP("sensing_mousex",              SENSING_MOUSEX)
    OP("sensing_mousey",              SENSING_MOUSEY)
    OP("sensing_timer",               SENSING_TIMER)
    OP("sensing_resettimer",          SENSING_RESETTIMER)
    OP("sensing_loudness",            SENSING_LOUDNESS)
    OP("sensing_askandwait",          SENSING_ASKANDWAIT)
    OP("sensing_of",                  SENSING_OF)
    OP("sensing_distanceto",          SENSING_DISTANCETO)
    OP("operator_add",                OPERATOR_ADD)
    OP("operator_subtract",           OPERATOR_SUBTRACT)
    OP("operator_multiply",           OPERATOR_MULTIPLY)
    OP("operator_divide",             OPERATOR_DIVIDE)
    OP("operator_random",             OPERATOR_RANDOM)
    OP("operator_gt",                 OPERATOR_GT)
    OP("operator_lt",                 OPERATOR_LT)
    OP("operator_equals",             OPERATOR_EQUALS)
    OP("operator_and",                OPERATOR_AND)
    OP("operator_or",                 OPERATOR_OR)
    OP("operator_not",                OPERATOR_NOT)
    OP("operator_join",               OPERATOR_JOIN)
    OP("operator_letter_of",          OPERATOR_LETTER_OF)
    OP("operator_length",             OPERATOR_LENGTH)
    OP("operator_mod",                OPERATOR_MOD)
    OP("operator_round",              OPERATOR_ROUND)
    OP("operator_mathop",             OPERATOR_MATHOP)
    OP("data_setvariableto",          DATA_SETVARIABLETO)
    OP("data_changevariableby",       DATA_CHANGEVARIABLEBY)
    OP("data_showvariable",           DATA_SHOWVARIABLE)
    OP("data_hidevariable",           DATA_HIDEVARIABLE)
    OP("data_addtolist",              DATA_ADDTOLIST)
    OP("data_deleteoflist",           DATA_DELETEOFLIST)
    OP("data_insertatlist",           DATA_INSERTATLIST)
    OP("data_replaceitemoflist",      DATA_REPLACEITEMOFLIST)
    OP("data_itemoflist",             DATA_ITEMOFLIST)
    OP("data_itemnumoflist",          DATA_ITEMNUMOFLIST)
    OP("data_lengthoflist",           DATA_LENGTHOFLIST)
    OP("data_listcontainsitem",       DATA_LISTCONTAINSITEM)
    OP("data_showlist",               DATA_SHOWLIST)
    OP("data_hidelist",               DATA_HIDELIST)
    OP("nds_buttonpressed",           NDS_BUTTONPRESSED)
    OP("nds_buttonheld",              NDS_BUTTONHELD)
    OP("nds_buttonreleased",          NDS_BUTTONRELEASED)
    OP("nds_touchx",                  NDS_TOUCHX)
    OP("nds_touchy",                  NDS_TOUCHY)
    OP("nds_touchpressed",            NDS_TOUCHPRESSED)
    OP("nds_microphone_loudness",     NDS_MICROPHONE_LOUDNESS)
    OP("nds_microphone_recording",    NDS_MICROPHONE_RECORDING)
    OP("nds_rumble",                  NDS_RUMBLE)
    OP("nds_setvibration",            NDS_SETVIBRATION)
    OP("nds_backlight_top",           NDS_BACKLIGHT_TOP)
    OP("nds_backlight_bottom",        NDS_BACKLIGHT_BOTTOM)

#undef OP
    return BlockOpcode::UNKNOWN;
}