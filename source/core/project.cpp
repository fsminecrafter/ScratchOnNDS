// =============================================================================
// project.cpp — Parses Scratch 3.0 project.json using jsmn
// =============================================================================
#define JSMN_STATIC
#include "jsmn.h"

#include "project.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    // For objects/arrays we must skip all their children
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
    // primitives and strings: already advanced by i++
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
// -----------------------------------------------------------------------
void ScratchProject::parseBlocks(const char* json, jsmntok_t* toks,
                                  int& i, int numToks,
                                  std::vector<ScratchBlock>& blocks) {
    if (i >= numToks || toks[i].type != JSMN_OBJECT) {
        skipValue(toks, i, numToks); return;
    }
    int n = toks[i].size; i++;
    for (int b = 0; b < n && i < numToks; b++) {
        std::string blockId = jsmnStr(json, &toks[i]); i++;
        if (i >= numToks || toks[i].type != JSMN_OBJECT) {
            skipValue(toks, i, numToks); continue;
        }
        ScratchBlock block;
        block.id = blockId;
        block.topLevel = false;
        block.shadow   = false;
        int nk = toks[i].size; i++;
        for (int k = 0; k < nk && i < numToks; k++) {
            std::string key = jsmnStr(json, &toks[i]); i++;
            if (i >= numToks) break;
            if (key == "opcode") {
                block.opcodeStr = jsmnStr(json, &toks[i]);
                block.opcode    = opcodeFromStr(block.opcodeStr);
                i++;
            } else if (key == "next") {
                // next can be null or string
                if (toks[i].type == JSMN_STRING)
                    block.nextId = jsmnStr(json, &toks[i]);
                i++;
            } else if (key == "parent") {
                if (toks[i].type == JSMN_STRING)
                    block.parentId = jsmnStr(json, &toks[i]);
                i++;
            } else if (key == "topLevel") {
                block.topLevel = jsmnBool(json, &toks[i]); i++;
            } else if (key == "shadow") {
                block.shadow = jsmnBool(json, &toks[i]); i++;
            } else if (key == "inputs") {
                parseBlockInputs(json, toks, i, numToks, block.inputs);
            } else if (key == "fields") {
                parseBlockFields(json, toks, i, numToks, block.fields);
            } else {
                skipValue(toks, i, numToks);
            }
        }
        blocks.push_back(std::move(block));
    }
}

// -----------------------------------------------------------------------
// Parse block inputs object
// -----------------------------------------------------------------------
void ScratchProject::parseBlockInputs(const char* json, jsmntok_t* toks,
                                       int& i, int numToks,
                                       std::map<std::string, ScratchInput>& inputs) {
    if (i >= numToks || toks[i].type != JSMN_OBJECT) {
        skipValue(toks, i, numToks); return;
    }
    int n = toks[i].size; i++;
    for (int k = 0; k < n && i < numToks; k++) {
        std::string inputName = jsmnStr(json, &toks[i]); i++;
        ScratchInput inp{};
        // Input value is an array: [shadowType, value_or_blockId]
        if (i < numToks && toks[i].type == JSMN_ARRAY) {
            int arrSz = toks[i].size; i++;
            // element 0: shadow int
            if (arrSz > 0 && i < numToks) { i++; } // skip shadow indicator
            // element 1: block ID string or literal value array
            if (arrSz > 1 && i < numToks) {
                if (toks[i].type == JSMN_STRING) {
                    inp.blockId = jsmnStr(json, &toks[i]); i++;
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
        inputs[inputName] = inp;
    }
}

// -----------------------------------------------------------------------
// Parse block fields object
// -----------------------------------------------------------------------
void ScratchProject::parseBlockFields(const char* json, jsmntok_t* toks,
                                       int& i, int numToks,
                                       std::map<std::string, std::string>& fields) {
    if (i >= numToks || toks[i].type != JSMN_OBJECT) {
        skipValue(toks, i, numToks); return;
    }
    int n = toks[i].size; i++;
    for (int k = 0; k < n && i < numToks; k++) {
        std::string fieldName = jsmnStr(json, &toks[i]); i++;
        std::string fieldVal;
        // Field value is an array: [value, id_or_null]
        if (i < numToks && toks[i].type == JSMN_ARRAY) {
            int sz = toks[i].size; i++;
            if (sz > 0 && i < numToks) {
                fieldVal = jsmnStr(json, &toks[i]); i++;
            }
            for (int v = 1; v < sz && i < numToks; v++)
                skipValue(toks, i, numToks);
        } else {
            skipValue(toks, i, numToks);
        }
        fields[fieldName] = fieldVal;
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
        std::string varId = jsmnStr(json, &toks[i]); i++;
        ScratchVariable var{};
        var.id = varId;
        // Variable value is an array: [name, value, optional:isCloud]
        if (i < numToks && toks[i].type == JSMN_ARRAY) {
            int sz = toks[i].size; i++;
            if (sz > 0 && i < numToks) { var.name  = jsmnStr(json, &toks[i]); i++; }
            if (sz > 1 && i < numToks) { var.value = jsmnStr(json, &toks[i]); i++; }
            for (int v = 2; v < sz && i < numToks; v++)
                skipValue(toks, i, numToks);
        } else {
            skipValue(toks, i, numToks);
        }
        vars.push_back(var);
    }
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
// -----------------------------------------------------------------------
BlockOpcode ScratchProject::opcodeFromStr(const std::string& s) {
    static const struct { const char* str; BlockOpcode op; } map[] = {
        {"event_whenflagclicked",       BlockOpcode::EVENT_WHENFLAGCLICKED},
        {"event_whenkeypressed",        BlockOpcode::EVENT_WHENKEYPRESSED},
        {"event_whenthisspriteclicked", BlockOpcode::EVENT_WHENTHISSPRITECLICKED},
        {"event_whenbroadcastreceived", BlockOpcode::EVENT_WHENBROADCASTRECEIVED},
        {"motion_movesteps",            BlockOpcode::MOTION_MOVESTEPS},
        {"motion_turnright",            BlockOpcode::MOTION_TURNRIGHT},
        {"motion_turnleft",             BlockOpcode::MOTION_TURNLEFT},
        {"motion_gotoxy",               BlockOpcode::MOTION_GOTOXY},
        {"motion_glideto",              BlockOpcode::MOTION_GLIDETO},
        {"motion_setx",                 BlockOpcode::MOTION_SETX},
        {"motion_sety",                 BlockOpcode::MOTION_SETY},
        {"motion_changexby",            BlockOpcode::MOTION_CHANGEXBY},
        {"motion_changeyby",            BlockOpcode::MOTION_CHANGEYBY},
        {"motion_ifonedgebounce",       BlockOpcode::MOTION_IFONEDGEBOUNCE},
        {"looks_sayforsecs",            BlockOpcode::LOOKS_SAYFORSECS},
        {"looks_say",                   BlockOpcode::LOOKS_SAY},
        {"looks_switchcostumeto",       BlockOpcode::LOOKS_SWITCHCOSTUMETO},
        {"looks_nextcostume",           BlockOpcode::LOOKS_NEXTCOSTUME},
        {"looks_switchbackdropto",      BlockOpcode::LOOKS_SWITCHBACKDROPTO},
        {"looks_changesizeby",          BlockOpcode::LOOKS_CHANGESIZEBY},
        {"looks_setsizeto",             BlockOpcode::LOOKS_SETSIZETO},
        {"looks_show",                  BlockOpcode::LOOKS_SHOW},
        {"looks_hide",                  BlockOpcode::LOOKS_HIDE},
        {"sound_playuntildone",         BlockOpcode::SOUND_PLAYUNTILDONE},
        {"sound_play",                  BlockOpcode::SOUND_PLAY},
        {"sound_stopallsounds",         BlockOpcode::SOUND_STOPALLSOUNDS},
        {"control_wait",                BlockOpcode::CONTROL_WAIT},
        {"control_repeat",              BlockOpcode::CONTROL_REPEAT},
        {"control_forever",             BlockOpcode::CONTROL_FOREVER},
        {"control_if",                  BlockOpcode::CONTROL_IF},
        {"control_if_else",             BlockOpcode::CONTROL_IF_ELSE},
        {"control_wait_until",          BlockOpcode::CONTROL_WAIT_UNTIL},
        {"control_repeat_until",        BlockOpcode::CONTROL_REPEAT_UNTIL},
        {"control_stop",                BlockOpcode::CONTROL_STOP},
        {"control_start_as_clone",      BlockOpcode::CONTROL_START_AS_CLONE},
        {"control_create_clone_of",     BlockOpcode::CONTROL_CREATE_CLONE_OF},
        {"control_delete_this_clone",   BlockOpcode::CONTROL_DELETE_THIS_CLONE},
        {"sensing_touchingobject",      BlockOpcode::SENSING_TOUCHINGOBJECT},
        {"sensing_keypressed",          BlockOpcode::SENSING_KEYPRESSED},
        {"sensing_mousedown",           BlockOpcode::SENSING_MOUSEDOWN},
        {"sensing_mousex",              BlockOpcode::SENSING_MOUSEX},
        {"sensing_mousey",              BlockOpcode::SENSING_MOUSEY},
        {"sensing_timer",               BlockOpcode::SENSING_TIMER},
        {"sensing_resettimer",          BlockOpcode::SENSING_RESETTIMER},
        {"sensing_loudness",            BlockOpcode::SENSING_LOUDNESS},
        {"sensing_askandwait",          BlockOpcode::SENSING_ASKANDWAIT},
        {"operator_add",                BlockOpcode::OPERATOR_ADD},
        {"operator_subtract",           BlockOpcode::OPERATOR_SUBTRACT},
        {"operator_multiply",           BlockOpcode::OPERATOR_MULTIPLY},
        {"operator_divide",             BlockOpcode::OPERATOR_DIVIDE},
        {"operator_random",             BlockOpcode::OPERATOR_RANDOM},
        {"operator_gt",                 BlockOpcode::OPERATOR_GT},
        {"operator_lt",                 BlockOpcode::OPERATOR_LT},
        {"operator_equals",             BlockOpcode::OPERATOR_EQUALS},
        {"operator_and",                BlockOpcode::OPERATOR_AND},
        {"operator_or",                 BlockOpcode::OPERATOR_OR},
        {"operator_not",                BlockOpcode::OPERATOR_NOT},
        {"operator_join",               BlockOpcode::OPERATOR_JOIN},
        {"operator_mod",                BlockOpcode::OPERATOR_MOD},
        {"operator_round",              BlockOpcode::OPERATOR_ROUND},
        {"operator_mathop",             BlockOpcode::OPERATOR_MATHOP},
        {"data_setvariableto",          BlockOpcode::DATA_SETVARIABLETO},
        {"data_changevariableby",       BlockOpcode::DATA_CHANGEVARIABLEBY},
        {"nds_buttonpressed",           BlockOpcode::NDS_BUTTONPRESSED},
        {"nds_buttonheld",              BlockOpcode::NDS_BUTTONHELD},
        {"nds_buttonreleased",          BlockOpcode::NDS_BUTTONRELEASED},
        {"nds_touchx",                  BlockOpcode::NDS_TOUCHX},
        {"nds_touchy",                  BlockOpcode::NDS_TOUCHY},
        {"nds_touchpressed",            BlockOpcode::NDS_TOUCHPRESSED},
        {"nds_microphone_loudness",     BlockOpcode::NDS_MICROPHONE_LOUDNESS},
        {"nds_rumble",                  BlockOpcode::NDS_RUMBLE},
        {"nds_setvibration",            BlockOpcode::NDS_SETVIBRATION},
        {"nds_backlight_top",           BlockOpcode::NDS_BACKLIGHT_TOP},
        {"nds_backlight_bottom",        BlockOpcode::NDS_BACKLIGHT_BOTTOM},
    };
    for (auto& m : map) {
        if (s == m.str) return m.op;
    }
    return BlockOpcode::UNKNOWN;
}
