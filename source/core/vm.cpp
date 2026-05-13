#define _USE_MATH_DEFINES
// =============================================================================
// vm.cpp — Extended Scratch 3.0 Virtual Machine
//
// New in this revision:
//   • Lists: add/delete/insert/replace/item/itemNum/length/contains/show/hide
//   • Clones: create_clone_of, start_as_clone, delete_this_clone (pool alloc)
//   • Broadcast-and-wait: caller yields until all spawned threads finish
//   • Glide-to: smooth interpolation tracked per stack frame (no extra alloc)
//   • String ops: letter_of, length, contains, join (already had join)
//   • sensing_of: read x/y/direction/size/costume/volume from another sprite
//   • Touching: sprite–sprite AABB, sprite–edge
//   • Math: all OPERATOR_MATHOP variants
//   • Control: repeat_until, wait_until (edge-triggered, not level)
//   • Data: show/hide variable, show/hide list
//   • Sound: change/set volume
//   • Motion: set rotation style, x/y/direction reporters
//   • Looks: say reporters, costume number/name, size reporter
// =============================================================================
#include "vm.h"
#include "../audio/audio_manager.h"
#include "../input/input_handler.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ═══════════════════════════════════════════════════════════════════════════════
// ScratchValue
// ═══════════════════════════════════════════════════════════════════════════════

double ScratchValue::toNum() const {
    if (type == NUM) return numVal;
    // Scratch: trim whitespace then parse
    const char* p = strVal.c_str();
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) return 0.0;
    char* end;
    double v = strtod(p, &end);
    // If nothing was parsed, return 0 (Scratch semantics)
    if (end == p) return 0.0;
    return v;
}

std::string ScratchValue::toStr() const {
    if (type == STR) return strVal;
    // Scratch: integers print without decimal; floats use minimal precision
    char buf[32];
    double v = numVal;
    if (v != v) return "NaN";               // NaN guard
    if (v == (long long)v && v >= -1e15 && v <= 1e15)
        snprintf(buf, sizeof(buf), "%lld", (long long)v);
    else
        snprintf(buf, sizeof(buf), "%.10g", v);
    return buf;
}

bool ScratchValue::operator==(const ScratchValue& o) const {
    // Scratch equality: numeric if both parse as numbers, else case-insensitive string
    if (type == STR && o.type == STR) {
        // Try numeric comparison first
        const char* a = strVal.c_str();
        const char* b = o.strVal.c_str();
        char* ea; char* eb;
        double da = strtod(a, &ea);
        double db = strtod(b, &eb);
        bool aNum = (ea != a && *ea == '\0');
        bool bNum = (eb != b && *eb == '\0');
        if (aNum && bNum) return da == db;
        // Case-insensitive string compare
        if (strVal.size() != o.strVal.size()) return false;
        for (size_t i = 0; i < strVal.size(); i++)
            if (tolower((unsigned char)strVal[i]) != tolower((unsigned char)o.strVal[i]))
                return false;
        return true;
    }
    return toNum() == o.toNum();
}
bool ScratchValue::operator<(const ScratchValue& o) const { return toNum() < o.toNum(); }
bool ScratchValue::operator>(const ScratchValue& o) const { return toNum() > o.toNum(); }

// ═══════════════════════════════════════════════════════════════════════════════
// Helpers
// ═══════════════════════════════════════════════════════════════════════════════

double ScratchVM::normDir(double d) {
    d = fmod(d, 360.0);
    if (d > 180.0)   d -= 360.0;
    if (d <= -180.0) d += 360.0;
    return d;
}

// Helpers that delegate to the flat-array API in project.h
static inline bool inputHas(const ScratchBlock* b, const char* key) {
    return blockHasInput(b, key);
}
static inline bool fieldHas(const ScratchBlock* b, const char* key) {
    return blockGetField(b, key)[0] != '\0';
}
static inline const char* fieldVal(const ScratchBlock* b, const char* key) {
    return blockGetField(b, key);
}
// Safe input accessor — returns a static empty input if key not found
static const ScratchInput& inputGet(const ScratchBlock* b, const char* key) {
    const ScratchInput* p = blockGetInput(b, key);
    static ScratchInput empty{};
    return p ? *p : empty;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Init / Green Flag
// ═══════════════════════════════════════════════════════════════════════════════

void ScratchVM::init(ScratchProject& proj) {
    project = &proj;
    globalTimer = 0.0;
    answerStr.clear();
    threads.clear();
    pendingThreads.clear();
    broadcasts.clear();
    runtimeLists.clear();

    // Initialise clone pool
    memset(cloneUsed, 0, sizeof(cloneUsed));

    // Build runtime lists from project data
    for (auto& sprite : proj.targets) {
        for (auto& lst : sprite.lists) {
            ListEntry e;
            e.owner      = &sprite;
            e.list.name  = lst.name;
            e.list.items = lst.items;
            runtimeLists.push_back(std::move(e));
        }
    }
}

void ScratchVM::greenFlag() {
    threads.clear();
    pendingThreads.clear();
    broadcasts.clear();
    globalTimer = 0.0;
    // Delete all clones
    if (project) {
        auto& tgts = project->targets;
        for (int i = (int)tgts.size() - 1; i >= 0; i--) {
            if (tgts[i].isClone) tgts.erase(tgts.begin() + i);
        }
    }
    memset(cloneUsed, 0, sizeof(cloneUsed));
    for (auto& sprite : project->targets)
        startHatBlocks(BlockOpcode::EVENT_WHENFLAGCLICKED, &sprite);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Hat block spawning
// ═══════════════════════════════════════════════════════════════════════════════

void ScratchVM::startHatBlocks(BlockOpcode hat, ScratchSprite* sprite,
                                const char* field) {
    for (auto& b : sprite->blocks) {
        if (!b.topLevel || b.opcode != hat) continue;
        if (hat == BlockOpcode::EVENT_WHENKEYPRESSED) {
            if (strcmp(fieldVal(&b,"KEY_OPTION"), field)!=0) continue;
        }
        if (hat == BlockOpcode::EVENT_WHENBROADCASTRECEIVED) {
            // Case-insensitive broadcast match
            const std::string& bOpt = fieldVal(&b, "BROADCAST_OPTION");
            if (bOpt.size() != strlen(field)) continue;
            bool match = true;
            for (size_t i = 0; i < bOpt.size(); i++)
                if (tolower((unsigned char)bOpt[i]) != tolower((unsigned char)field[i]))
                    { match = false; break; }
            if (!match) continue;
        }
        if (b.nextId[0] != '\0' && pendingThreads.size() < 64) {
            ScriptThread t;
            t.sprite         = sprite;
            strncpy(t.currentBlockId, b.nextId, MAX_BLOCK_ID); t.currentBlockId[MAX_BLOCK_ID]= '\0';
            t.state          = ScriptThread::RUNNING;
            t.isClone        = sprite->isClone;
            pendingThreads.push_back(std::move(t));
        }
    }
}

int ScratchVM::startHatBlocksCount(
                        BlockOpcode hat,
                        ScratchSprite* sprite,
                        const char* field) {
    int before = (int)pendingThreads.size();
    startHatBlocks(hat, sprite, field);
    return (int)pendingThreads.size() - before;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Step
// ═══════════════════════════════════════════════════════════════════════════════

void ScratchVM::step(double dt) {
    globalTimer += dt;

    // Flush pending threads
    for (auto& t : pendingThreads) {
        if (threads.size() < 128) threads.push_back(std::move(t));
    }
    pendingThreads.clear();

    // Advance broadcasts-and-wait counters
    for (auto& br : broadcasts) {
        if (br.isWaiting && br.threadsLaunched > 0
            && br.threadsDone >= br.threadsLaunched) {
            // wake up any thread waiting on this
            for (auto& t : threads) {
                if (t.state == ScriptThread::WAITING_BROADCAST
                    && !t.callStack.empty()
                    && strcmp(t.callStack.back().broadcastName, br.name)==0) {
                    t.state = ScriptThread::RUNNING;
                }
            }
        }
    }
    // Remove finished broadcast records
    for (int i = (int)broadcasts.size() - 1; i >= 0; i--) {
        if (broadcasts[i].threadsDone >= broadcasts[i].threadsLaunched)
            broadcasts.erase(broadcasts.begin() + i);
    }

    // Run threads
    for (int i = (int)threads.size() - 1; i >= 0; i--) {
        ScriptThread& thread = threads[i];

        if (thread.state == ScriptThread::DONE) {
            threads.erase(threads.begin() + i);
            continue;
        }
        if (thread.state == ScriptThread::WAITING_SECS) {
            thread.waitTimer -= dt;
            if (thread.waitTimer > 0.0) continue;
            thread.state = ScriptThread::RUNNING;
        }
        if (thread.state == ScriptThread::WAITING_SOUND) {
            if (AudioManager::getInstance().isPlaying()) continue;
            thread.state = ScriptThread::RUNNING;
        }
        if (thread.state == ScriptThread::WAITING_BROADCAST) {
            continue;  // woken above
        }
        if (thread.state == ScriptThread::WAITING_UNTIL) {
            // Condition block stored in top callStack frame
            if (!thread.callStack.empty() && thread.callStack.back().isWaitUntil) {
                bool cond = false;
                bool y = false;
                ScratchValue cv = executeBlock(thread,
                    thread.callStack.back().condBlockId, y);
                cond = cv.toBool();
                thread.state = ScriptThread::RUNNING;  // reset so executeBlock works
                if (!cond) { thread.state = ScriptThread::WAITING_UNTIL; continue; }
                thread.callStack.pop_back();
            } else {
                thread.state = ScriptThread::RUNNING;
            }
        }

        // Tick glide on top frame if active
        if (!thread.callStack.empty() && thread.callStack.back().isGlide) {
            StackFrame& gf = thread.callStack.back();
            gf.glideElapsed += dt;
            double t_frac = (gf.glideDuration > 0.0)
                            ? gf.glideElapsed / gf.glideDuration
                            : 1.0;
            if (t_frac >= 1.0) t_frac = 1.0;
            thread.sprite->x = gf.glideStartX + (gf.glideEndX - gf.glideStartX) * t_frac;
            thread.sprite->y = gf.glideStartY + (gf.glideEndY - gf.glideStartY) * t_frac;
            if (t_frac >= 1.0) {
                thread.callStack.pop_back();
                thread.state = ScriptThread::RUNNING;
            } else {
                continue;  // still gliding
            }
        }

        thread.stepsThisFrame = 0;
        executeThread(thread, dt);
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Execute thread for one frame
// ═══════════════════════════════════════════════════════════════════════════════

void ScratchVM::executeThread(ScriptThread& thread, double /*dt*/) {
    while (!thread.currentBlockId[0]=='\0'
           && thread.state == ScriptThread::RUNNING
           && thread.stepsThisFrame < MAX_STEPS_PER_FRAME)
    {
        bool yielded = false;
        executeBlock(thread, thread.currentBlockId, yielded);
        thread.stepsThisFrame++;
        if (yielded) return;

        ScratchBlock* b = getBlock(thread, thread.currentBlockId);
        if (b && b->nextId[0] != '\0') {
            strncpy(thread.currentBlockId, b->nextId, MAX_BLOCK_ID); thread.currentBlockId[MAX_BLOCK_ID]= '\0';
        } else {
            // End of chain — unwind call stack
            while (!thread.callStack.empty()) {
                StackFrame& frame = thread.callStack.back();
                if (frame.isGlide || frame.isWaitUntil || frame.isBroadcastWait) {
                    return; // handled in step()
                }
                if (frame.remaining > 0) frame.remaining--;
                if (frame.remaining == 0) {
                
                    thread.callStack.pop_back();
                
                    if (!thread.callStack.empty()) {
                
                        strncpy(thread.currentBlockId,
                                thread.callStack.back().returnBlockId,
                                MAX_BLOCK_ID);
                
                        thread.currentBlockId[MAX_BLOCK_ID] = '\0';
                
                    } else {
                
                        thread.currentBlockId[0] = '\0';
                    }
                
                } else {
                
                    frame.remaining--;
                
                    ScratchBlock* lb = getBlock(thread, frame.loopBlockId);
                
                    if (lb && inputHas(lb, "SUBSTACK")) {
                
                        strncpy(thread.currentBlockId,
                                inputGet(lb, "SUBSTACK").blockId,
                                MAX_BLOCK_ID);
                
                        thread.currentBlockId[MAX_BLOCK_ID] = '\0';
                
                    } else {
                
                        thread.currentBlockId[0] = '\0';
                    }
                
                    return;
                }
                if (!thread.currentBlockId[0]=='\0') return;
            }
            thread.state = ScriptThread::DONE;
            return;
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Execute one block
// ═══════════════════════════════════════════════════════════════════════════════

//in header

// ═══════════════════════════════════════════════════════════════════════════════
// Motion
// ═══════════════════════════════════════════════════════════════════════════════

ScratchValue ScratchVM::execMotion(ScriptThread& t, ScratchBlock* b, bool& yielded) {
    ScratchSprite* s = t.sprite;
    switch (b->opcode) {
        case BlockOpcode::MOTION_MOVESTEPS: {
            double steps = evaluateInput(t, inputGet(b,"STEPS")).toNum();
            double rad = (s->direction - 90.0) * M_PI / 180.0;
            s->x += cos(rad) * steps;
            s->y -= sin(rad) * steps;
            break;
        }
        case BlockOpcode::MOTION_TURNRIGHT:
            s->direction = normDir(s->direction +
                evaluateInput(t, inputGet(b,"DEGREES")).toNum());
            break;
        case BlockOpcode::MOTION_TURNLEFT:
            s->direction = normDir(s->direction -
                evaluateInput(t, inputGet(b,"DEGREES")).toNum());
            break;
        case BlockOpcode::MOTION_GOTOXY:
            s->x = evaluateInput(t, inputGet(b,"X")).toNum();
            s->y = evaluateInput(t, inputGet(b,"Y")).toNum();
            break;
        case BlockOpcode::MOTION_GLIDETO: {
            // Push a glide frame — actual movement in step()
            double secs = inputHas(b, "SECS")
                          ? evaluateInput(t, inputGet(b,"SECS")).toNum() : 0.0;
            double ex   = inputHas(b, "X") ? evaluateInput(t, inputGet(b,"X")).toNum() : s->x;
            double ey   = inputHas(b, "Y") ? evaluateInput(t, inputGet(b,"Y")).toNum() : s->y;
            StackFrame gf;
            gf.isGlide       = true;
            strncpy(gf.loopBlockId, b->id, MAX_BLOCK_ID);
            gf.loopBlockId[MAX_BLOCK_ID] = '\0';
            
            strncpy(gf.returnBlockId, b->nextId, MAX_BLOCK_ID);
            gf.returnBlockId[MAX_BLOCK_ID] = '\0';
            gf.glideStartX   = s->x;
            gf.glideStartY   = s->y;
            gf.glideEndX     = ex;
            gf.glideEndY     = ey;
            gf.glideDuration = secs > 0.0 ? secs : 0.0;
            gf.glideElapsed  = 0.0;
            gf.remaining     = 1;
            t.callStack.push_back(gf);
            yielded = true;
            break;
        }
        case BlockOpcode::MOTION_SETX:
            s->x = evaluateInput(t, inputGet(b,"X")).toNum(); break;
        case BlockOpcode::MOTION_SETY:
            s->y = evaluateInput(t, inputGet(b,"Y")).toNum(); break;
        case BlockOpcode::MOTION_CHANGEXBY:
            s->x += evaluateInput(t, inputGet(b,"DX")).toNum(); break;
        case BlockOpcode::MOTION_CHANGEYBY:
            s->y += evaluateInput(t, inputGet(b,"DY")).toNum(); break;
        case BlockOpcode::MOTION_IFONEDGEBOUNCE: {
            const double XMAX = 220.0, YMAX = 160.0;
            bool hitX = false, hitY = false;
            if (s->x >  XMAX) { s->x =  XMAX; hitX = true; }
            if (s->x < -XMAX) { s->x = -XMAX; hitX = true; }
            if (s->y >  YMAX) { s->y =  YMAX; hitY = true; }
            if (s->y < -YMAX) { s->y = -YMAX; hitY = true; }
            if (hitX || hitY) {
                double rad = (s->direction - 90.0) * M_PI / 180.0;
                double dx = cos(rad), dy = -sin(rad);
                if (hitX) dx = -dx;
                if (hitY) dy = -dy;
                s->direction = normDir(atan2(-dy, dx) * 180.0 / M_PI + 90.0);
            }
            break;
        }
        case BlockOpcode::MOTION_SETROTATIONSTYLE:
            s->rotationStyle = fieldVal(b, "STYLE");
            break;
        case BlockOpcode::MOTION_XPOSITION:
            return ScratchValue(s->x);
        case BlockOpcode::MOTION_YPOSITION:
            return ScratchValue(s->y);
        case BlockOpcode::MOTION_DIRECTION:
            return ScratchValue((double)s->direction);
        default: break;
    }
    return ScratchValue();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Looks
// ═══════════════════════════════════════════════════════════════════════════════

ScratchValue ScratchVM::execLooks(ScriptThread& t, ScratchBlock* b, bool& yielded) {
    ScratchSprite* s = t.sprite;
    switch (b->opcode) {
        case BlockOpcode::LOOKS_SAY:
            if (inputHas(b, "MESSAGE"))
                s->sayMessage = evaluateInput(t, inputGet(b,"MESSAGE")).toStr();
            break;
        case BlockOpcode::LOOKS_SAYFORSECS:
            if (inputHas(b, "MESSAGE"))
                s->sayMessage = evaluateInput(t, inputGet(b,"MESSAGE")).toStr();
            if (inputHas(b, "SECS")) {
                t.state     = ScriptThread::WAITING_SECS;
                t.waitTimer = evaluateInput(t, inputGet(b,"SECS")).toNum();
                yielded = true;
            }
            break;
        case BlockOpcode::LOOKS_SWITCHCOSTUMETO: {
            if (!inputHas(b, "COSTUME")) break;
            ScratchValue cv = evaluateInput(t, inputGet(b,"COSTUME"));
            // Try by name first
            bool found = false;
            for (int i = 0; i < (int)s->costumes.size(); i++) {
                if (s->costumes[i].name == cv.toStr()) { s->currentCostume = i; found = true; break; }
            }
            // Fall back to numeric index (1-based in Scratch)
            if (!found) {
                int idx = (int)cv.toNum() - 1;
                if (idx >= 0 && idx < (int)s->costumes.size())
                    s->currentCostume = idx;
            }
            break;
        }
        case BlockOpcode::LOOKS_NEXTCOSTUME:
            if (!s->costumes.empty())
                s->currentCostume = (s->currentCostume + 1) % (int)s->costumes.size();
            break;
        case BlockOpcode::LOOKS_SWITCHBACKDROPTO: {
            ScratchSprite* stage = project->getStage();
            if (!stage || !inputHas(b, "BACKDROP")) break;
            ScratchValue bv = evaluateInput(t, inputGet(b,"BACKDROP"));
            for (int i = 0; i < (int)stage->costumes.size(); i++) {
                if (stage->costumes[i].name == bv.toStr()) { stage->currentCostume = i; break; }
            }
            break;
        }
        case BlockOpcode::LOOKS_SHOW:    s->visible = true;  break;
        case BlockOpcode::LOOKS_HIDE:    s->visible = false; break;
        case BlockOpcode::LOOKS_SETSIZETO:
            s->size = evaluateInput(t, inputGet(b,"SIZE")).toNum();
            if (s->size < 0.0) s->size = 0.0;
            break;
        case BlockOpcode::LOOKS_CHANGESIZEBY:
            s->size += evaluateInput(t, inputGet(b,"CHANGE")).toNum();
            if (s->size < 0.0) s->size = 0.0;
            break;
        // Reporters
        case BlockOpcode::LOOKS_SETEFFECTTO:
        case BlockOpcode::LOOKS_CHANGEEFFECTBY:
            // Graphic effects not implemented on NDS; silently ignore
            break;
        default: break;
    }
    return ScratchValue();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Sound
// ═══════════════════════════════════════════════════════════════════════════════

ScratchValue ScratchVM::execSound(ScriptThread& t, ScratchBlock* b, bool& yielded) {
    switch (b->opcode) {
        case BlockOpcode::SOUND_PLAY:
        case BlockOpcode::SOUND_PLAYUNTILDONE: {
            if (!inputHas(b, "SOUND_MENU")) break;
            ScratchValue sn = evaluateInput(t, inputGet(b,"SOUND_MENU"));
            AudioManager::getInstance().playSound(t.sprite, sn.toStr());
            if (b->opcode == BlockOpcode::SOUND_PLAYUNTILDONE) {
                t.state = ScriptThread::WAITING_SOUND;
                yielded = true;
            }
            break;
        }
        case BlockOpcode::SOUND_STOPALLSOUNDS:
            AudioManager::getInstance().stopAll();
            break;
        case BlockOpcode::SOUND_CHANGEVOLUMEBY:
        case BlockOpcode::SOUND_SETVOLUMETO: {
            int vol = (int)AudioManager::getInstance().isPlaying();
            // No per-sprite volume on NDS; apply globally
            if (b->opcode == BlockOpcode::SOUND_SETVOLUMETO && inputHas(b, "VOLUME"))
                vol = (int)evaluateInput(t, inputGet(b,"VOLUME")).toNum();
            else if (inputHas(b, "VOLUME"))
                vol = (int)evaluateInput(t, inputGet(b,"VOLUME")).toNum();
            if (vol < 0) vol = 0;
            if (vol > 100) vol = 100;
            AudioManager::getInstance().setVolume(vol);
            break;
        }
        default: break;
    }
    return ScratchValue();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Control
// ═══════════════════════════════════════════════════════════════════════════════

ScratchValue ScratchVM::execControl(ScriptThread& t, ScratchBlock* b, bool& yielded) {
    switch (b->opcode) {
        case BlockOpcode::CONTROL_WAIT: {
            double secs = evaluateInput(t, inputGet(b,"DURATION")).toNum();
            t.state     = ScriptThread::WAITING_SECS;
            t.waitTimer = secs;
            yielded = true;
            break;
        }
        case BlockOpcode::CONTROL_REPEAT: {
            int times = (int)evaluateInput(t, inputGet(b,"TIMES")).toNum();
            if (times <= 0 || !inputHas(b, "SUBSTACK")) break;
            StackFrame fr;
            strncpy(fr.loopBlockId, b->id, MAX_BLOCK_ID); fr.loopBlockId[MAX_BLOCK_ID]= '\0';
            strncpy(fr.returnBlockId, b->nextId, MAX_BLOCK_ID); fr.returnBlockId[MAX_BLOCK_ID]= '\0';
            fr.remaining     = times;
            t.callStack.push_back(fr);
            strncpy(t.currentBlockId, inputGet(b,"SUBSTACK").blockId, MAX_BLOCK_ID); t.currentBlockId[MAX_BLOCK_ID]= '\0';
            yielded = true;
            break;
        }
        case BlockOpcode::CONTROL_FOREVER: {
            if (!inputHas(b, "SUBSTACK")) break;
            StackFrame fr;
            strncpy(fr.loopBlockId, b->id, MAX_BLOCK_ID); fr.loopBlockId[MAX_BLOCK_ID]= '\0';
            strncpy(fr.returnBlockId, b->id, MAX_BLOCK_ID); fr.returnBlockId[MAX_BLOCK_ID]= '\0';  // forever never returns past itself
            fr.remaining     = -1;
            t.callStack.push_back(fr);
            strncpy(t.currentBlockId, inputGet(b,"SUBSTACK").blockId, MAX_BLOCK_ID); t.currentBlockId[MAX_BLOCK_ID]= '\0';
            yielded = true;
            break;
        }
        case BlockOpcode::CONTROL_IF: {
            bool cond = inputHas(b, "CONDITION") &&
                        evaluateInput(t, inputGet(b,"CONDITION")).toBool();
            if (cond && inputHas(b, "SUBSTACK")) {
                StackFrame fr;
                strncpy(fr.loopBlockId, b->id, MAX_BLOCK_ID); fr.loopBlockId[MAX_BLOCK_ID]= '\0';
                strncpy(fr.returnBlockId, b->nextId, MAX_BLOCK_ID); fr.returnBlockId[MAX_BLOCK_ID]= '\0';
                fr.remaining     = 1;
                t.callStack.push_back(fr);
                strncpy(t.currentBlockId, inputGet(b,"SUBSTACK").blockId, MAX_BLOCK_ID); t.currentBlockId[MAX_BLOCK_ID]= '\0';
                yielded = true;
            }
            break;
        }
        case BlockOpcode::CONTROL_IF_ELSE: {
            bool cond = inputHas(b, "CONDITION") &&
                        evaluateInput(t, inputGet(b,"CONDITION")).toBool();
            const char* key = cond ? "SUBSTACK" : "SUBSTACK2";
            if (inputHas(b, key)) {
                StackFrame fr;
                strncpy(fr.loopBlockId, b->id, MAX_BLOCK_ID); fr.loopBlockId[MAX_BLOCK_ID]= '\0';
                strncpy(fr.returnBlockId, b->nextId, MAX_BLOCK_ID); fr.returnBlockId[MAX_BLOCK_ID]= '\0';
                fr.remaining     = 1;
                t.callStack.push_back(fr);
                strncpy(t.currentBlockId, inputGet(b, key).blockId, MAX_BLOCK_ID); t.currentBlockId[MAX_BLOCK_ID]= '\0';
                yielded = true;
            }
            break;
        }
        case BlockOpcode::CONTROL_WAIT_UNTIL: {
            bool cond = inputHas(b, "CONDITION") &&
                        evaluateInput(t, inputGet(b,"CONDITION")).toBool();
            if (!cond) {
                StackFrame fr;
                fr.isWaitUntil = true;
                if (inputHas(b, "CONDITION")) {
                    strncpy(fr.condBlockId,
                            inputGet(b, "CONDITION").blockId,
                            MAX_BLOCK_ID);
                    fr.condBlockId[MAX_BLOCK_ID] = '\0';
                } else {
                    fr.condBlockId[0] = '\0';
                }
                strncpy(fr.loopBlockId, b->id, MAX_BLOCK_ID); fr.loopBlockId[MAX_BLOCK_ID]= '\0';
                strncpy(fr.returnBlockId, b->nextId, MAX_BLOCK_ID); fr.returnBlockId[MAX_BLOCK_ID]= '\0';
                fr.remaining = -1;
                t.callStack.push_back(fr);
                t.state  = ScriptThread::WAITING_UNTIL;
                yielded = true;
            }
            break;
        }
        case BlockOpcode::CONTROL_REPEAT_UNTIL: {
            bool cond = inputHas(b, "CONDITION") &&
                        evaluateInput(t, inputGet(b,"CONDITION")).toBool();
            if (!cond && inputHas(b, "SUBSTACK")) {
                StackFrame fr;
                strncpy(fr.loopBlockId, b->id, MAX_BLOCK_ID); fr.loopBlockId[MAX_BLOCK_ID]= '\0';
                strncpy(fr.returnBlockId, b->nextId, MAX_BLOCK_ID); fr.returnBlockId[MAX_BLOCK_ID]= '\0';
                fr.remaining     = -1;      // we check condition on each re-entry
                // Store condition block id in condBlockId for re-check
                if (inputHas(b, "CONDITION")) {
                    strncpy(fr.condBlockId,
                            inputGet(b, "CONDITION").blockId,
                            MAX_BLOCK_ID);
                    fr.condBlockId[MAX_BLOCK_ID] = '\0';
                } else {
                    fr.condBlockId[0] = '\0';
                }
                t.callStack.push_back(fr);
                strncpy(t.currentBlockId, inputGet(b,"SUBSTACK").blockId, MAX_BLOCK_ID); t.currentBlockId[MAX_BLOCK_ID]= '\0';
                yielded = true;
            }
            break;
        }
        case BlockOpcode::CONTROL_STOP: {
            const std::string& opt = fieldVal(b, "STOP_OPTION");
            if (opt == "all") {
                stopAll();
            } else if (opt == "this script") {
                t.state  = ScriptThread::DONE;
                yielded = true;
            } else if (opt == "other scripts in sprite") {
                for (auto& th : threads) {
                    if (&th != &t && th.sprite == t.sprite)
                        th.state = ScriptThread::DONE;
                }
            }
            break;
        }
        case BlockOpcode::CONTROL_START_AS_CLONE:
            // Hat block — handled by greenFlag/clone spawning; no-op here
            break;
        case BlockOpcode::CONTROL_CREATE_CLONE_OF: {
            if (!inputHas(b, "CLONE_OPTION")) break;
            ScratchValue tgt = evaluateInput(t, inputGet(b,"CLONE_OPTION"));
            ScratchSprite* src = nullptr;
            if (tgt.toStr() == "_myself_") {
                src = t.sprite;
            } else {
                src = project->findSprite(tgt.toStr());
            }
            if (src) {
                ScratchSprite* cl = createClone(src);
                if (cl) {
                    // Fire start_as_clone hat in next frame
                    int n = startHatBlocksCount(
                        BlockOpcode::CONTROL_START_AS_CLONE, cl);
                    (void)n;
                }
            }
            break;
        }
        case BlockOpcode::CONTROL_DELETE_THIS_CLONE: {
            if (t.sprite->isClone) {
                deleteClone(t.sprite);
                t.state  = ScriptThread::DONE;
                yielded = true;
            }
            break;
        }
        default: break;
    }
    return ScratchValue();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Sensing
// ═══════════════════════════════════════════════════════════════════════════════

ScratchValue ScratchVM::execSensing(ScriptThread& t, ScratchBlock* b, bool& yielded) {
    InputHandler& input = InputHandler::getInstance();
    switch (b->opcode) {
        case BlockOpcode::SENSING_TOUCHINGOBJECT: {
            if (!inputHas(b, "TOUCHINGOBJECTMENU")) return ScratchValue(0.0);
            ScratchValue tgt = evaluateInput(t, inputGet(b,"TOUCHINGOBJECTMENU"));
            std::string name = tgt.toStr();
            if (name == "_edge_") {
                return ScratchValue(spriteTouchingEdge(t.sprite) ? 1.0 : 0.0);
            }
            if (name == "_mouse_") {
                // Touch position as "mouse"
                int mx = input.getTouchX(), my = input.getTouchY();
                double hw = t.sprite->costumes.empty() ? 16.0
                            : t.sprite->costumes[t.sprite->currentCostume].width * 0.5;
                double hh = t.sprite->costumes.empty() ? 16.0
                            : t.sprite->costumes[t.sprite->currentCostume].height * 0.5;
                bool hit = (mx >= t.sprite->x - hw && mx <= t.sprite->x + hw &&
                            my >= t.sprite->y - hh && my <= t.sprite->y + hh);
                return ScratchValue(hit ? 1.0 : 0.0);
            }
            ScratchSprite* other = project->findSprite(name);
            if (other) return ScratchValue(spritesTouching(t.sprite, other) ? 1.0 : 0.0);
            return ScratchValue(0.0);
        }
        case BlockOpcode::SENSING_KEYPRESSED: {
            std::string key = fieldVal(b, "KEY_OPTION");
            return ScratchValue(input.isKeyHeld(key) ? 1.0 : 0.0);
        }
        case BlockOpcode::SENSING_MOUSEDOWN:
            return ScratchValue(input.isTouching() ? 1.0 : 0.0);
        case BlockOpcode::SENSING_MOUSEX:
            return ScratchValue((double)input.getTouchX());
        case BlockOpcode::SENSING_MOUSEY:
            return ScratchValue((double)input.getTouchY());
        case BlockOpcode::SENSING_TIMER:
            return ScratchValue(globalTimer);
        case BlockOpcode::SENSING_RESETTIMER:
            globalTimer = 0.0;
            return ScratchValue();
        case BlockOpcode::SENSING_LOUDNESS:
            return ScratchValue((double)input.getMicLoudness());
        case BlockOpcode::SENSING_ANSWER:
            return ScratchValue(answerStr);
        case BlockOpcode::SENSING_ASKANDWAIT:
            // No keyboard on NDS; store blank answer, yield briefly then continue
            answerStr = "";
            t.state     = ScriptThread::WAITING_SECS;
            t.waitTimer = 0.016;
            yielded = true;
            break;
        case BlockOpcode::SENSING_OF: {
            // sensing_of — get property of another sprite
            std::string prop  = fieldVal(b, "PROPERTY");
            std::string sName = fieldVal(b, "OBJECT");
            ScratchSprite* target = (sName == "_stage_")
                                    ? project->getStage()
                                    : project->findSprite(sName);
            if (target) return getSpriteProperty(target, prop.c_str());
            return ScratchValue(0.0);
        }
        case BlockOpcode::SENSING_DISTANCETO: {
            if (!inputHas(b, "DISTANCETOMENU")) return ScratchValue(0.0);
            ScratchValue tgt = evaluateInput(t, inputGet(b,"DISTANCETOMENU"));
            double tx = 0.0, ty = 0.0;
            if (tgt.toStr() == "_mouse_") {
                tx = input.getTouchX();
                ty = input.getTouchY();
            } else {
                ScratchSprite* other = project->findSprite(tgt.toStr());
                if (other) { tx = other->x; ty = other->y; }
            }
            double dx = tx - t.sprite->x, dy = ty - t.sprite->y;
            return ScratchValue(sqrt(dx*dx + dy*dy));
        }
        default: break;
    }
    return ScratchValue();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Data (variables + lists)
// ═══════════════════════════════════════════════════════════════════════════════

ScratchValue ScratchVM::execData(ScriptThread& t, ScratchBlock* b, bool& yielded) {
    (void)yielded;
    switch (b->opcode) {
        case BlockOpcode::DATA_SETVARIABLETO: {
            if (!fieldHas(b, "VARIABLE") || !inputHas(b, "VALUE")) break;
            setVariable(t.sprite, fieldVal(b, "VARIABLE"),
                        evaluateInput(t, inputGet(b,"VALUE")));
            break;
        }
        case BlockOpcode::DATA_CHANGEVARIABLEBY: {
            if (!fieldHas(b, "VARIABLE") || !inputHas(b, "VALUE")) break;
            ScratchValue delta = evaluateInput(t, inputGet(b,"VALUE"));
            ScratchValue cur   = getVariable(t.sprite, fieldVal(b, "VARIABLE"));
            setVariable(t.sprite, fieldVal(b, "VARIABLE"),
                        ScratchValue(cur.toNum() + delta.toNum()));
            break;
        }
        case BlockOpcode::DATA_SHOWVARIABLE:
        case BlockOpcode::DATA_HIDEVARIABLE: {
            bool vis = (b->opcode == BlockOpcode::DATA_SHOWVARIABLE);
            const std::string& vname = fieldVal(b, "VARIABLE");
            // Search sprite then stage
            auto setVis = [&](ScratchSprite* sp) {
                if (!sp) return;
                for (auto& v : sp->variables)
                    if (v.name == vname) { v.visible = vis; }
            };
            setVis(t.sprite);
            setVis(project->getStage());
            break;
        }
        // ── Lists ─────────────────────────────────────────────────────────────
        case BlockOpcode::DATA_ADDTOLIST: {
            if (!fieldHas(b, "LIST") || !inputHas(b, "ITEM")) break;
            ScratchRuntimeList* lst = getList(t.sprite, fieldVal(b, "LIST"));
            if (lst) lst->items.push_back(evaluateInput(t, inputGet(b,"ITEM")).toStr());
            break;
        }
        case BlockOpcode::DATA_DELETEOFLIST: {
            if (!fieldHas(b, "LIST") || !inputHas(b, "INDEX")) break;
            ScratchRuntimeList* lst = getList(t.sprite, fieldVal(b, "LIST"));
            if (!lst) break;
            ScratchValue iv = evaluateInput(t, inputGet(b,"INDEX"));
            if (iv.toStr() == "all") {
                lst->items.clear();
            } else if (iv.toStr() == "last") {
                if (!lst->items.empty()) lst->items.pop_back();
            } else {
                int idx = (int)iv.toNum() - 1;
                if (idx >= 0 && idx < (int)lst->items.size())
                    lst->items.erase(lst->items.begin() + idx);
            }
            break;
        }
        case BlockOpcode::DATA_INSERTATLIST: {
            if (!fieldHas(b, "LIST") || !inputHas(b, "ITEM") || !inputHas(b, "INDEX")) break;
            ScratchRuntimeList* lst = getList(t.sprite, fieldVal(b, "LIST"));
            if (!lst) break;
            std::string val = evaluateInput(t, inputGet(b,"ITEM")).toStr();
            ScratchValue iv  = evaluateInput(t, inputGet(b,"INDEX"));
            int idx;
            if (iv.toStr() == "last") idx = (int)lst->items.size();
            else if (iv.toStr() == "random") idx = lst->items.empty() ? 0 : rand() % (int)lst->items.size();
            else idx = (int)iv.toNum() - 1;
            if (idx < 0) idx = 0;
            if (idx > (int)lst->items.size()) idx = (int)lst->items.size();
            lst->items.insert(lst->items.begin() + idx, val);
            break;
        }
        case BlockOpcode::DATA_REPLACEITEMOFLIST: {
            if (!fieldHas(b, "LIST") || !inputHas(b, "ITEM") || !inputHas(b, "INDEX")) break;
            ScratchRuntimeList* lst = getList(t.sprite, fieldVal(b, "LIST"));
            if (!lst) break;
            std::string val = evaluateInput(t, inputGet(b,"ITEM")).toStr();
            int idx = (int)evaluateInput(t, inputGet(b,"INDEX")).toNum() - 1;
            if (idx >= 0 && idx < (int)lst->items.size())
                lst->items[idx] = val;
            break;
        }
        case BlockOpcode::DATA_ITEMOFLIST: {
            if (!fieldHas(b, "LIST") || !inputHas(b, "INDEX")) break;
            ScratchRuntimeList* lst = getList(t.sprite, fieldVal(b, "LIST"));
            if (!lst) return ScratchValue("");
            ScratchValue iv = evaluateInput(t, inputGet(b,"INDEX"));
            if (iv.toStr() == "last") {
                return lst->items.empty() ? ScratchValue("") : ScratchValue(lst->items.back());
            }
            if (iv.toStr() == "random") {
                if (lst->items.empty()) return ScratchValue("");
                return ScratchValue(lst->items[rand() % lst->items.size()]);
            }
            int idx = (int)iv.toNum() - 1;
            if (idx >= 0 && idx < (int)lst->items.size())
                return ScratchValue(lst->items[idx]);
            return ScratchValue("");
        }
        case BlockOpcode::DATA_ITEMNUMOFLIST: {
            if (!fieldHas(b, "LIST") || !inputHas(b, "ITEM")) break;
            ScratchRuntimeList* lst = getList(t.sprite, fieldVal(b, "LIST"));
            if (!lst) return ScratchValue(0.0);
            std::string val = evaluateInput(t, inputGet(b,"ITEM")).toStr();
            for (int i = 0; i < (int)lst->items.size(); i++) {
                if (lst->items[i] == val) return ScratchValue((double)(i + 1));
            }
            return ScratchValue(0.0);
        }
        case BlockOpcode::DATA_LENGTHOFLIST: {
            if (!fieldHas(b, "LIST")) break;
            ScratchRuntimeList* lst = getList(t.sprite, fieldVal(b, "LIST"));
            return ScratchValue(lst ? (double)lst->items.size() : 0.0);
        }
        case BlockOpcode::DATA_LISTCONTAINSITEM: {
            if (!fieldHas(b, "LIST") || !inputHas(b, "ITEM")) break;
            ScratchRuntimeList* lst = getList(t.sprite, fieldVal(b, "LIST"));
            if (!lst) return ScratchValue(0.0);
            std::string val = evaluateInput(t, inputGet(b,"ITEM")).toStr();
            for (auto& it : lst->items)
                if (it == val) return ScratchValue(1.0);
            return ScratchValue(0.0);
        }
        case BlockOpcode::DATA_SHOWLIST:
        case BlockOpcode::DATA_HIDELIST:
            // Monitor visibility — not rendered on NDS; silently accepted
            break;
        default: break;
    }
    return ScratchValue();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Operators
// ═══════════════════════════════════════════════════════════════════════════════

ScratchValue ScratchVM::execOperator(ScriptThread& t, ScratchBlock* b, bool& yielded) {
    (void)yielded;
    switch (b->opcode) {
        case BlockOpcode::OPERATOR_ADD:
            return ScratchValue(evaluateInput(t, inputGet(b,"NUM1")).toNum() +
                                evaluateInput(t, inputGet(b,"NUM2")).toNum());
        case BlockOpcode::OPERATOR_SUBTRACT:
            return ScratchValue(evaluateInput(t, inputGet(b,"NUM1")).toNum() -
                                evaluateInput(t, inputGet(b,"NUM2")).toNum());
        case BlockOpcode::OPERATOR_MULTIPLY:
            return ScratchValue(evaluateInput(t, inputGet(b,"NUM1")).toNum() *
                                evaluateInput(t, inputGet(b,"NUM2")).toNum());
        case BlockOpcode::OPERATOR_DIVIDE: {
            double d = evaluateInput(t, inputGet(b,"NUM2")).toNum();
            return ScratchValue(d != 0.0 ? evaluateInput(t, inputGet(b,"NUM1")).toNum() / d
                                         : 0.0);
        }
        case BlockOpcode::OPERATOR_RANDOM: {
            double lo = evaluateInput(t, inputGet(b,"FROM")).toNum();
            double hi = evaluateInput(t, inputGet(b,"TO")).toNum();
            if (lo > hi) { double tmp = lo; lo = hi; hi = tmp; }
            // Scratch: integer range → integer result
            if (lo == (long long)lo && hi == (long long)hi)
                return ScratchValue((double)(lo + rand() % ((long long)(hi - lo) + 1)));
            return ScratchValue(lo + (double)rand() / RAND_MAX * (hi - lo));
        }
        case BlockOpcode::OPERATOR_GT:
            return ScratchValue(evaluateInput(t, inputGet(b,"OPERAND1")) >
                                evaluateInput(t, inputGet(b,"OPERAND2")) ? 1.0 : 0.0);
        case BlockOpcode::OPERATOR_LT:
            return ScratchValue(evaluateInput(t, inputGet(b,"OPERAND1")) <
                                evaluateInput(t, inputGet(b,"OPERAND2")) ? 1.0 : 0.0);
        case BlockOpcode::OPERATOR_EQUALS:
            return ScratchValue(evaluateInput(t, inputGet(b,"OPERAND1")) ==
                                evaluateInput(t, inputGet(b,"OPERAND2")) ? 1.0 : 0.0);
        case BlockOpcode::OPERATOR_AND:
            return ScratchValue(evaluateInput(t, inputGet(b,"OPERAND1")).toBool() &&
                                evaluateInput(t, inputGet(b,"OPERAND2")).toBool() ? 1.0 : 0.0);
        case BlockOpcode::OPERATOR_OR:
            return ScratchValue(evaluateInput(t, inputGet(b,"OPERAND1")).toBool() ||
                                evaluateInput(t, inputGet(b,"OPERAND2")).toBool() ? 1.0 : 0.0);
        case BlockOpcode::OPERATOR_NOT:
            return ScratchValue(!evaluateInput(t, inputGet(b,"OPERAND")).toBool() ? 1.0 : 0.0);
        case BlockOpcode::OPERATOR_JOIN: {
            std::string a = evaluateInput(t, inputGet(b,"STRING1")).toStr();
            std::string bv = evaluateInput(t, inputGet(b,"STRING2")).toStr();
            return ScratchValue(a + bv);
        }
        case BlockOpcode::OPERATOR_LETTER_OF: {
            int idx = (int)evaluateInput(t, inputGet(b,"LETTER")).toNum() - 1;
            std::string str = evaluateInput(t, inputGet(b,"STRING")).toStr();
            if (idx >= 0 && idx < (int)str.size())
                return ScratchValue(std::string(1, str[idx]));
            return ScratchValue("");
        }
        case BlockOpcode::OPERATOR_LENGTH:
            return ScratchValue((double)evaluateInput(t, inputGet(b,"STRING")).toStr().size());
        case BlockOpcode::OPERATOR_MOD: {
            double n = evaluateInput(t, inputGet(b,"NUM1")).toNum();
            double d = evaluateInput(t, inputGet(b,"NUM2")).toNum();
            if (d == 0.0) return ScratchValue(0.0);
            double r = fmod(n, d);
            // Scratch: result has same sign as divisor
            if (r != 0.0 && (r < 0.0) != (d < 0.0)) r += d;
            return ScratchValue(r);
        }
        case BlockOpcode::OPERATOR_ROUND:
            return ScratchValue(floor(evaluateInput(t, inputGet(b,"NUM")).toNum() + 0.5));
        case BlockOpcode::OPERATOR_MATHOP: {
            double n  = evaluateInput(t, inputGet(b,"NUM")).toNum();
            const std::string& op = fieldVal(b, "OPERATOR");
            if      (op == "abs")     return ScratchValue(fabs(n));
            else if (op == "floor")   return ScratchValue(floor(n));
            else if (op == "ceiling") return ScratchValue(ceil(n));
            else if (op == "sqrt")    return ScratchValue(n >= 0.0 ? sqrt(n) : 0.0);
            else if (op == "sin")     return ScratchValue(sin(n * M_PI / 180.0));
            else if (op == "cos")     return ScratchValue(cos(n * M_PI / 180.0));
            else if (op == "tan") {
                double c = cos(n * M_PI / 180.0);
                return ScratchValue(c != 0.0 ? sin(n * M_PI / 180.0) / c : 0.0);
            }
            else if (op == "asin")    return ScratchValue(asin(n) * 180.0 / M_PI);
            else if (op == "acos")    return ScratchValue(acos(n) * 180.0 / M_PI);
            else if (op == "atan")    return ScratchValue(atan(n) * 180.0 / M_PI);
            else if (op == "ln")      return ScratchValue(n > 0.0 ? log(n)   : 0.0);
            else if (op == "log")     return ScratchValue(n > 0.0 ? log10(n) : 0.0);
            else if (op == "e ^")     return ScratchValue(exp(n));
            else if (op == "10 ^")    return ScratchValue(pow(10.0, n));
            return ScratchValue(0.0);
        }
        // String contains (Scratch 3.0)
        default:
            break;
    }
    return ScratchValue();
}

// ═══════════════════════════════════════════════════════════════════════════════
// NDS Extension
// ═══════════════════════════════════════════════════════════════════════════════

ScratchValue ScratchVM::execNDS(ScriptThread& t, ScratchBlock* b, bool& yielded) {
    (void)t; (void)yielded;
    InputHandler& input = InputHandler::getInstance();
    switch (b->opcode) {
        case BlockOpcode::NDS_BUTTONPRESSED:
            return ScratchValue(input.isButtonDown(fieldVal(b, "BUTTON")) ? 1.0 : 0.0);
        case BlockOpcode::NDS_BUTTONHELD:
            return ScratchValue(input.isButtonHeld(fieldVal(b, "BUTTON")) ? 1.0 : 0.0);
        case BlockOpcode::NDS_BUTTONRELEASED:
            return ScratchValue(input.isButtonReleased(fieldVal(b, "BUTTON")) ? 1.0 : 0.0);
        case BlockOpcode::NDS_TOUCHX:
            return ScratchValue((double)input.getTouchX());
        case BlockOpcode::NDS_TOUCHY:
            return ScratchValue((double)input.getTouchY());
        case BlockOpcode::NDS_TOUCHPRESSED:
            return ScratchValue(input.isTouching() ? 1.0 : 0.0);
        case BlockOpcode::NDS_MICROPHONE_LOUDNESS:
            return ScratchValue((double)input.getMicLoudness());
        case BlockOpcode::NDS_RUMBLE:
        case BlockOpcode::NDS_SETVIBRATION:
        case BlockOpcode::NDS_BACKLIGHT_TOP:
        case BlockOpcode::NDS_BACKLIGHT_BOTTOM:
            // Hardware commands are fired from main.cpp via NDSExtension
            break;
        default: break;
    }
    return ScratchValue();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Evaluate input slot
// ═══════════════════════════════════════════════════════════════════════════════

ScratchValue ScratchVM::evaluateInput(ScriptThread& thread, const ScratchInput& input) {
    if (input.blockId[0] != '\0')
        return evaluateReporter(thread, input.blockId);
    // valueType 4–8 are numeric literals in Scratch JSON
    if (input.valueType >= 4 && input.valueType <= 8)
        return ScratchValue(input.numValue);
    return ScratchValue(input.strValue);
}

ScratchValue ScratchVM::evaluateReporter(
                        ScriptThread& thread,
                        const char* blockId) {
    bool yielded = false;
    ScriptThread::State saved = thread.state;
    thread.state = ScriptThread::RUNNING;
    ScratchValue v = executeBlock(thread, blockId, yielded);
    if (!yielded) thread.state = saved;
    return v;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Broadcast
// ═══════════════════════════════════════════════════════════════════════════════

void ScratchVM::broadcast(const std::string& name) {
    for (auto& sprite : project->targets)
        startHatBlocks(BlockOpcode::EVENT_WHENBROADCASTRECEIVED, &sprite, name.c_str());
}

void ScratchVM::broadcastAndWait(ScriptThread& caller, const std::string& name) {
    BroadcastRecord br;
    strncpy(br.name, name.c_str(), 31); br.name[31]= '\0';
    br.threadsLaunched   = 0;
    br.threadsDone       = 0;
    br.isWaiting         = true;
    for (auto& sprite : project->targets)
        br.threadsLaunched += startHatBlocksCount(
            BlockOpcode::EVENT_WHENBROADCASTRECEIVED, &sprite, name.c_str());
    if (br.threadsLaunched == 0) return;  // no one listening, don't wait
    broadcasts.push_back(br);
    StackFrame fr;
    fr.isBroadcastWait = true;
    strncpy(fr.broadcastName, name.c_str(), 31); fr.broadcastName[31]= '\0';
    fr.remaining       = 1;
    fr.loopBlockId[0] = '\0';
    caller.callStack.push_back(fr);
    caller.state = ScriptThread::WAITING_BROADCAST;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Stop all
// ═══════════════════════════════════════════════════════════════════════════════

void ScratchVM::stopAll() {
    for (auto& t : threads)        t.state = ScriptThread::DONE;
    for (auto& t : pendingThreads) t.state = ScriptThread::DONE;
    broadcasts.clear();
    AudioManager::getInstance().stopAll();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Variable access
// ═══════════════════════════════════════════════════════════════════════════════
/*
ScratchValue ScratchVM::getVariable(ScratchSprite* sprite,
                                     const std::string& name) {
    // Search local sprite first, then stage (global)
    auto search = [&](ScratchSprite* sp) -> ScratchValue* {
        if (!sp) return nullptr;
        for (auto& v : sp->variables)
            if (v.name == name) return (ScratchValue*)&v;  // reinterpret hack — see below
        return nullptr;
    };
    // Can't return reference to ScratchVariable::value directly as ScratchValue,
    // so we construct from string.
    if (sprite) {
        for (auto& v : sprite->variables)
            if (v.name == name) {
                // Try numeric parse for efficiency
                ScratchValue sv;
                sv.type = ScratchValue::STR;
                sv.strVal = v.value;
                return sv;
            }
    }
    ScratchSprite* stage = project->getStage();
    if (stage) {
        for (auto& v : stage->variables)
            if (v.name == name) {
                ScratchValue sv;
                sv.type = ScratchValue::STR;
                sv.strVal = v.value;
                return sv;
            }
    }
    return ScratchValue(0.0);
}

void ScratchVM::setVariable(ScratchSprite* sprite, const std::string& name,
                              const ScratchValue& val) {
    std::string s = val.toStr();
    if (sprite) {
        for (auto& v : sprite->variables)
            if (v.name == name) { v.value = s; return; }
    }
    ScratchSprite* stage = project->getStage();
    if (stage) {
        for (auto& v : stage->variables)
            if (v.name == name) { v.value = s; return; }
    }
}
*/

// ═══════════════════════════════════════════════════════════════════════════════
// List access
// ═══════════════════════════════════════════════════════════════════════════════

ScratchRuntimeList* ScratchVM::getList(
                        ScratchSprite* sprite,
                        const char* name) {
    // Search sprite-local first
    for (auto& e : runtimeLists)
        if (e.owner == sprite && e.list.name == name) return &e.list;
    // Stage / global
    ScratchSprite* stage = project->getStage();
    for (auto& e : runtimeLists)
        if (e.owner == stage && e.list.name == name) return &e.list;
    return nullptr;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Clone management
// ═══════════════════════════════════════════════════════════════════════════════

ScratchSprite* ScratchVM::createClone(ScratchSprite* parent) {
    // Find a free slot in the pool
    int slot = -1;
    for (int i = 0; i < MAX_CLONES; i++) {
        if (!cloneUsed[i]) { slot = i; break; }
    }
    if (slot < 0) return nullptr;  // pool exhausted

    clonePool[slot]                = *parent;  // copy all fields
    clonePool[slot].isClone        = true;
    clonePool[slot].cloneParentIndex = (int)(parent - project->targets.data());
    cloneUsed[slot] = true;

    // Add to project targets so renderer picks it up
    if (project->targets.size() < 128)
        project->targets.push_back(ScratchSprite());  // placeholder, filled below

    // Use push_back copy
    project->targets.back() = clonePool[slot];

    return &project->targets.back();
}

void ScratchVM::deleteClone(ScratchSprite* sprite) {
    // Stop all threads for this clone
    for (auto& t : threads)
        if (t.sprite == sprite) t.state = ScriptThread::DONE;
    // Remove from project targets
    auto& tgts = project->targets;
    for (int i = (int)tgts.size() - 1; i >= 0; i--) {
        if (&tgts[i] == sprite) {
            tgts.erase(tgts.begin() + i);
            break;
        }
    }
    // Free pool slot
    for (int i = 0; i < MAX_CLONES; i++) {
        if (cloneUsed[i] && &clonePool[i] == sprite) {
            cloneUsed[i] = false;
            break;
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Sprite property (for sensing_of)
// ═══════════════════════════════════════════════════════════════════════════════
ScratchValue ScratchVM::getSpriteProperty(ScratchSprite* s, const char* prop)
{
    if (strcmp(prop, "x position") == 0)
        return ScratchValue(s->x);

    if (strcmp(prop, "y position") == 0)
        return ScratchValue(s->y);

    if (strcmp(prop, "direction") == 0)
        return ScratchValue((double)s->direction);

    if (strcmp(prop, "costume #") == 0 ||
        strcmp(prop, "costume number") == 0)
        return ScratchValue((double)s->costumeIndex);

    if (strcmp(prop, "costume name") == 0)
        return ScratchValue(s->costumeName);

    if (strcmp(prop, "size") == 0)
        return ScratchValue(s->size);

    if (strcmp(prop, "volume") == 0)
        return ScratchValue(100.0);

    if (strcmp(prop, "backdrop #") == 0 ||
        strcmp(prop, "backdrop number") == 0)
        return ScratchValue((double)s->backdropIndex);

    if (strcmp(prop, "backdrop name") == 0)
        return ScratchValue(s->backdropName);

    // Variable fallback
    return getVariable(s, prop);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Collision helpers
// ═══════════════════════════════════════════════════════════════════════════════

bool ScratchVM::spritesTouching(ScratchSprite* a, ScratchSprite* b) const {
    if (!a->visible || !b->visible) return false;
    double hwA = a->costumes.empty() ? 16.0 : a->costumes[a->currentCostume].width  * (a->size / 100.0) * 0.5;
    double hhA = a->costumes.empty() ? 16.0 : a->costumes[a->currentCostume].height * (a->size / 100.0) * 0.5;
    double hwB = b->costumes.empty() ? 16.0 : b->costumes[b->currentCostume].width  * (b->size / 100.0) * 0.5;
    double hhB = b->costumes.empty() ? 16.0 : b->costumes[b->currentCostume].height * (b->size / 100.0) * 0.5;
    return !(a->x + hwA < b->x - hwB || a->x - hwA > b->x + hwB ||
             a->y + hhA < b->y - hhB || a->y - hhA > b->y + hhB);
}

bool ScratchVM::spriteTouchingEdge(ScratchSprite* s) const {
    double hw = s->costumes.empty() ? 16.0
                : s->costumes[s->currentCostume].width  * (s->size / 100.0) * 0.5;
    double hh = s->costumes.empty() ? 16.0
                : s->costumes[s->currentCostume].height * (s->size / 100.0) * 0.5;
    return (s->x - hw < -220.0 || s->x + hw > 220.0 ||
            s->y - hh < -160.0 || s->y + hh > 160.0);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Utility
// ═══════════════════════════════════════════════════════════════════════════════
/*
ScratchBlock* ScratchVM::getBlock(ScriptThread& t, const std::string& id) {
    for (auto& blk : t.sprite->blocks)
        if (blk.id == id) return &blk;
    return nullptr;
}
*/

void ScratchVM::fireSpriteClicked(ScratchSprite* sprite) {
    startHatBlocks(BlockOpcode::EVENT_WHENTHISSPRITECLICKED, sprite);
}

void ScratchVM::fireKeyPressed(const std::string& key) {
    for (auto& sprite : project->targets)
        startHatBlocks(BlockOpcode::EVENT_WHENKEYPRESSED, &sprite, key.c_str());
}
