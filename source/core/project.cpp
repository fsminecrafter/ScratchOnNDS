// =============================================================================
// project.cpp — Parses Scratch 3.0 project.json using jsmn
// =============================================================================
#include "project.h"
#include "jsmn.h"   // drop jsmn.h into project (single header tokenizer)
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
    if (!tok || tok->type != JSMN_STRING) return "";
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

    bool ok = parseJson(json, sz);
    free(json);
    return ok;
}

// -----------------------------------------------------------------------
// Main JSON parser — walks jsmn token stream
// -----------------------------------------------------------------------
bool ScratchProject::parseJson(const char* json, size_t len) {
    jsmn_parser p;
    jsmn_init(&p);

    // Count tokens first
    int numToks = jsmn_parse(&p, json, len, nullptr, 0);
    if (numToks < 0) return false;

    jsmntok_t* toks = (jsmntok_t*)malloc(numToks * sizeof(jsmntok_t));
    if (!toks) return false;

    jsmn_init(&p);
    jsmn_parse(&p, json, len, toks, numToks);

    // Root must be object
    if (toks[0].type != JSMN_OBJECT) { free(toks); return false; }

    int i = 1;
    while (i < numToks) {
        if (jsmnEq(json, &toks[i], "meta")) {
            // meta object
            i++;
            if (toks[i].type == JSMN_OBJECT) {
                int metaKeys = toks[i].size;
                i++;
                for (int k = 0; k < metaKeys; k++) {
                    std::string key = jsmnStr(json, &toks[i]);
                    i++;
                    if (key == "semver") meta.semver = jsmnStr(json, &toks[i]);
                    else if (key == "vm")  meta.vm    = jsmnStr(json, &toks[i]);
                    else if (key == "agent") meta.agent = jsmnStr(json, &toks[i]);
                    i++;
                }
            } else i++;
        }
        else if (jsmnEq(json, &toks[i], "targets")) {
            i++;
            if (toks[i].type == JSMN_ARRAY) {
                int numTargets = toks[i].size;
                i++;
                for (int t = 0; t < numTargets; t++) {
                    ScratchSprite sprite;
                    parseTarget(json, toks, i, numToks, sprite);
                    targets.push_back(sprite);
                    // Skip past this target's tokens
                    // (parseTarget advances i internally via reference)
                }
            } else i++;
        }
        else {
            // Skip unknown key/value pair
            i++;
            if (i < numToks) {
                int skip = 1;
                if (toks[i].type == JSMN_OBJECT || toks[i].type == JSMN_ARRAY) {
                    skip = toks[i].size * 2 + 1;
                }
                i += skip;
            }
        }
    }

    free(toks);
    return !targets.empty();
}

// -----------------------------------------------------------------------
// Parse a single target (sprite or stage)
// NOTE: In a full implementation, i would be passed by reference and
//       advanced.  This is a simplified but functional version.
// -----------------------------------------------------------------------
void ScratchProject::parseTarget(const char* json, jsmntok_t* toks,
                                  int& i, int numToks, ScratchSprite& sprite) {
    if (i >= numToks || toks[i].type != JSMN_OBJECT) return;
    int numKeys = toks[i].size;
    i++;

    sprite.visible = true;
    sprite.x = sprite.y = 0;
    sprite.size = 100;
    sprite.direction = 90;
    sprite.currentCostume = 0;
    sprite.isStage = false;
    sprite.isClone = false;
    sprite.oamId = -1;

    for (int k = 0; k < numKeys; k++) {
        if (i >= numToks) break;
        std::string key = jsmnStr(json, &toks[i]); i++;
        if (i >= numToks) break;

        if (key == "isStage") {
            sprite.isStage = strncmp(json + toks[i].start, "true", 4) == 0;
            i++;
        } else if (key == "name") {
            sprite.name = jsmnStr(json, &toks[i]); i++;
        } else if (key == "x") {
            sprite.x = jsmnNum(json, &toks[i]); i++;
        } else if (key == "y") {
            sprite.y = jsmnNum(json, &toks[i]); i++;
        } else if (key == "size") {
            sprite.size = jsmnNum(json, &toks[i]); i++;
        } else if (key == "direction") {
            sprite.direction = (int)jsmnNum(json, &toks[i]); i++;
        } else if (key == "visible") {
            sprite.visible = strncmp(json + toks[i].start, "true", 4) == 0;
            i++;
        } else if (key == "currentCostume") {
            sprite.currentCostume = (int)jsmnNum(json, &toks[i]); i++;
        } else if (key == "rotationStyle") {
            sprite.rotationStyle = jsmnStr(json, &toks[i]); i++;
        } else if (key == "layerOrder") {
            sprite.layerOrder = (int)jsmnNum(json, &toks[i]); i++;
        } else if (key == "costumes") {
            parseCostumes(json, toks, i, numToks, sprite.costumes);
        } else if (key == "sounds") {
            parseSounds(json, toks, i, numToks, sprite.sounds);
        } else if (key == "blocks") {
            parseBlocks(json, toks, i, numToks, sprite.blocks);
        } else if (key == "variables") {
            parseVariables(json, toks, i, numToks, sprite.variables);
        } else {
            // Skip unknown value
            skipValue(toks, i, numToks);
        }
    }
}

void ScratchProject::parseCostumes(const char* json, jsmntok_t* toks,
                                    int& i, int numToks,
                                    std::vector<ScratchCostume>& costumes) {
    if (i >= numToks || toks[i].type != JSMN_ARRAY) { skipValue(toks, i, numToks); return; }
    int n = toks[i].size; i++;
    for (int c = 0; c < n; c++) {
        ScratchCostume costume;
        costume.bitmapResolution = 1;
        costume.rotationCenterX = costume.rotationCenterY = 0;
        costume.gfxPtr = costume.palPtr = nullptr;
        costume.width = costume.height = 0;
        if (i >= numToks || toks[i].type != JSMN_OBJECT) { skipValue(toks, i, numToks); continue; }
        int nk = toks[i].size; i++;
        for (int k = 0; k < nk; k++) {
            std::string key = jsmnStr(json, &toks[i]); i++;
            if (key == "name")             { costume.name = jsmnStr(json, &toks[i]); i++; }
            else if (key == "assetId")     { costume.assetId = jsmnStr(json, &toks[i]); i++; }
            else if (key == "dataFormat")  { costume.dataFormat = jsmnStr(json, &toks[i]); i++; }
            else if (key == "bitmapResolution") { costume.bitmapResolution = (int)jsmnNum(json, &toks[i]); i++; }
            else if (key == "rotationCenterX")  { costume.rotationCenterX = jsmnNum(json, &toks[i]); i++; }
            else if (key == "rotationCenterY")  { costume.rotationCenterY = jsmnNum(json, &toks[i]); i++; }
            else { skipValue(toks, i, numToks); }
        }
        costumes.push_back(costume);
    }
}

void ScratchProject::parseSounds(const char* json, jsmntok_t* toks,
                                  int& i, int numToks,
                                  std::vector<ScratchSound>& sounds) {
    if (i >= numToks || toks[i].type != JSMN_ARRAY) { skipValue(toks, i, numToks); return; }
    int n = toks[i].size; i++;
    for (int s = 0; s < n; s++) {
        ScratchSound sound;
        sound.rate = 44100; sound.sampleCount = 0;
        sound.loaded = false; sound.isStreamed = false;
        sound.pcmData = nullptr; sound.pcmSize = 0; sound.mmSoundId = -1;
        if (i >= numToks || toks[i].type != JSMN_OBJECT) { skipValue(toks, i, numToks); continue; }
        int nk = toks[i].size; i++;
        for (int k = 0; k < nk; k++) {
            std::string key = jsmnStr(json, &toks[i]); i++;
            if (key == "name")           { sound.name = jsmnStr(json, &toks[i]); i++; }
            else if (key == "assetId")   { sound.assetId = jsmnStr(json, &toks[i]); i++; }
            else if (key == "dataFormat"){ sound.dataFormat = jsmnStr(json, &toks[i]); i++; }
            else if (key == "rate")      { sound.rate = jsmnNum(json, &toks[i]); i++; }
            else if (key == "sampleCount"){ sound.sampleCount = (int)jsmnNum(json, &toks[i]); i++; }
            else { skipValue(toks, i, numToks); }
        }
        sounds.push_back(sound);
    }
}

void ScratchProject::parseBlocks(const char* json, jsmntok_t* toks,
                                  int& i, int numToks,
                                  std::map<std::string, ScratchBlock>& blocks) {
    if (i >= numToks || toks[i].type != JSMN_OBJECT) { skipValue(toks, i, numToks); return; }
    int n = toks[i].size; i++;
    for (int b = 0; b < n; b++) {
        std::string blockId = jsmnStr(json, &toks[i]); i++;
        ScratchBlock block;
        block.id = blockId;
        block.topLevel = false; block.shadow = false;
        if (i >= numToks || toks[i].type != JSMN_OBJECT) { skipValue(toks, i, numToks); continue; }
        int nk = toks[i].size; i++;
        for (int k = 0; k < nk; k++) {
            std::string key = jsmnStr(json, &toks[i]); i++;
            if (key == "opcode")   { block.opcodeStr = jsmnStr(json, &toks[i]); block.opcode = opcodeFromStr(block.opcodeStr); i++; }
            else if (key == "next") { block.nextId = jsmnStr(json, &toks[i]); i++; }
            else if (key == "parent") { block.parentId = jsmnStr(json, &toks[i]); i++; }
            else if (key == "topLevel") { block.topLevel = strncmp(json + toks[i].start, "true", 4) == 0; i++; }
            else if (key == "shadow") { block.shadow = strncmp(json + toks[i].start, "true", 4) == 0; i++; }
            else if (key == "inputs") {
                parseBlockInputs(json, toks, i, numToks, block.inputs);
            }
            else if (key == "fields") {
                parseBlockFields(json, toks, i, numToks, block.fields);
            }
            else { skipValue(toks, i, numToks); }
        }
        blocks[blockId] = block;
    }
}

void ScratchProject::parseBlockInputs(const char* json, jsmntok_t* toks,
                                       int& i, int numToks,
                                       std::map<std::string, ScratchInput>& inputs) {
    if (i >= numToks || toks[i].type != JSMN_OBJECT) { skipValue(toks, i, numToks); return; }
    int n = toks[i].size; i++;
    for (int k = 0; k < n; k++) {
        std::string inputName = jsmnStr(json, &toks[i]); i++;
        ScratchInput inp;
        inp.isShadow = false; inp.valueType = 0; inp.numValue = 0;
        // Input is an array: [shadowType, value]
        if (i < numToks && toks[i].type == JSMN_ARRAY) {
            int arrSz = toks[i].size; i++;
            if (arrSz > 0) {
                // First element: shadow indicator
                i++; // skip shadow int
                if (arrSz > 1) {
                    // Second element: block ID (string) or literal value array
                    if (toks[i].type == JSMN_STRING) {
                        inp.blockId = jsmnStr(json, &toks[i]); i++;
                    } else if (toks[i].type == JSMN_ARRAY) {
                        int valSz = toks[i].size; i++;
                        if (valSz >= 2) {
                            inp.valueType = (int)jsmnNum(json, &toks[i]); i++;
                            inp.strValue  = jsmnStr(json, &toks[i]); i++;
                            inp.numValue  = atof(inp.strValue.c_str());
                            for (int v = 2; v < valSz; v++) { skipValue(toks, i, numToks); }
                        }
                    } else { skipValue(toks, i, numToks); }
                    // Skip remaining input array elements
                    for (int v = 2; v < arrSz; v++) { skipValue(toks, i, numToks); }
                }
            }
        } else { skipValue(toks, i, numToks); }
        inputs[inputName] = inp;
    }
}

void ScratchProject::parseBlockFields(const char* json, jsmntok_t* toks,
                                       int& i, int numToks,
                                       std::map<std::string, std::string>& fields) {
    if (i >= numToks || toks[i].type != JSMN_OBJECT) { skipValue(toks, i, numToks); return; }
    int n = toks[i].size; i++;
    for (int k = 0; k < n; k++) {
        std::string fieldName = jsmnStr(json, &toks[i]); i++;
        std::string fieldVal;
        if (i < numToks && toks[i].type == JSMN_ARRAY) {
            int sz = toks[i].size; i++;
            if (sz > 0) { fieldVal = jsmnStr(json, &toks[i]); i++; }
            for (int v = 1; v < sz; v++) skipValue(toks, i, numToks);
        } else { skipValue(toks, i, numToks); }
        fields[fieldName] = fieldVal;
    }
}

void ScratchProject::parseVariables(const char* json, jsmntok_t* toks,
                                     int& i, int numToks,
                                     std::map<std::string, ScratchVariable>& vars) {
    if (i >= numToks || toks[i].type != JSMN_OBJECT) { skipValue(toks, i, numToks); return; }
    int n = toks[i].size; i++;
    for (int k = 0; k < n; k++) {
        std::string varId = jsmnStr(json, &toks[i]); i++;
        ScratchVariable var;
        var.id = varId; var.isCloud = false; var.visible = false;
        if (i < numToks && toks[i].type == JSMN_ARRAY) {
            int sz = toks[i].size; i++;
            if (sz > 0) { var.name  = jsmnStr(json, &toks[i]); i++; }
            if (sz > 1) { var.value = jsmnStr(json, &toks[i]); i++; }
            for (int v = 2; v < sz; v++) skipValue(toks, i, numToks);
        } else { skipValue(toks, i, numToks); }
        vars[varId] = var;
    }
}

// -----------------------------------------------------------------------
// Skip over a single JSON value (any type)
// -----------------------------------------------------------------------
void ScratchProject::skipValue(jsmntok_t* toks, int& i, int numToks) {
    if (i >= numToks) return;
    int end = toks[i].end;
    i++;
    while (i < numToks && toks[i].start < end) i++;
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
// Map opcode string -> enum
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
        // NDS Extension
        {"nds_buttonpressed",           BlockOpcode::NDS_BUTTONPRESSED},
        {"nds_buttonheld",              BlockOpcode::NDS_BUTTONHELD},
        {"nds_buttonreleased",          BlockOpcode::NDS_BUTTONRELEASED},
        {"nds_touchx",                  BlockOpcode::NDS_TOUCHX},
        {"nds_touchy",                  BlockOpcode::NDS_TOUCHY},
        {"nds_touchpressed",            BlockOpcode::NDS_TOUCHPRESSED},
        {"nds_microphone_loudness",     BlockOpcode::NDS_MICROPHONE_LOUDNESS},
        {"nds_rumble",                  BlockOpcode::NDS_RUMBLE},
    };
    for (auto& m : map) {
        if (s == m.str) return m.op;
    }
    return BlockOpcode::UNKNOWN;
}
