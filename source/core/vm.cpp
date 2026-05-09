#define _USE_MATH_DEFINES
// =============================================================================
// vm.cpp — Scratch VM execution engine
// =============================================================================
#include "vm.h"
#include "../audio/audio_manager.h"
#include "../input/input_handler.h"
#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// -----------------------------------------------------------------------
// ScratchValue helpers
// -----------------------------------------------------------------------
double ScratchValue::toNum() const {
    if (type == NUM) return numVal;
    return atof(strVal.c_str());
}
std::string ScratchValue::toStr() const {
    if (type == STR) return strVal;
    char buf[32];
    if (numVal == (long long)numVal) snprintf(buf, sizeof(buf), "%lld", (long long)numVal);
    else snprintf(buf, sizeof(buf), "%.10g", numVal);
    return buf;
}
bool ScratchValue::operator==(const ScratchValue& o) const {
    double a = toNum(), b = o.toNum();
    if (type == STR && o.type == STR) return strVal == o.strVal;
    return a == b;
}
bool ScratchValue::operator<(const ScratchValue& o) const { return toNum() < o.toNum(); }
bool ScratchValue::operator>(const ScratchValue& o) const { return toNum() > o.toNum(); }

// -----------------------------------------------------------------------
// Init
// -----------------------------------------------------------------------
void ScratchVM::init(ScratchProject& proj) {
    project = &proj;
    globalTimer = 0.0;
    threads.clear();
}

// -----------------------------------------------------------------------
// Green flag: start all "when flag clicked" hats
// -----------------------------------------------------------------------
void ScratchVM::greenFlag() {
    threads.clear();
    globalTimer = 0.0;
    for (auto& sprite : project->targets) {
        startHatBlocks(BlockOpcode::EVENT_WHENFLAGCLICKED, &sprite);
    }
}

// -----------------------------------------------------------------------
// Start threads for a hat block type on a sprite
// -----------------------------------------------------------------------
void ScratchVM::startHatBlocks(BlockOpcode hat, ScratchSprite* sprite,
                                const std::string& field) {
    for (auto& kv : sprite->blocks) {
        ScratchBlock& b = kv.second;
        if (!b.topLevel) continue;
        if (b.opcode != hat) continue;
        // For key/broadcast hats, check field matches
        if (hat == BlockOpcode::EVENT_WHENKEYPRESSED) {
            auto it = b.fields.find("KEY_OPTION");
            if (it == b.fields.end() || it->second != field) continue;
        }
        if (hat == BlockOpcode::EVENT_WHENBROADCASTRECEIVED) {
            auto it = b.fields.find("BROADCAST_OPTION");
            if (it == b.fields.end() || it->second != field) continue;
        }

        ScriptThread thread;
        thread.sprite = sprite;
        thread.currentBlockId = b.nextId;  // first block after hat
        thread.state = ScriptThread::RUNNING;
        thread.waitTimer = 0.0;
        thread.isClone = false;
        thread.stepsThisFrame = 0;
        if (!thread.currentBlockId.empty())
            threads.push_back(thread);
    }
}

// -----------------------------------------------------------------------
// Step: advance all threads by dt seconds
// -----------------------------------------------------------------------
void ScratchVM::step(double dt) {
    globalTimer += dt;

    // Process pending threads from last frame
    for (auto& t : pendingThreads) threads.push_back(t);
            if (threads.size() < 64)  // hard cap — NDS can't handle more
            threads.push_back(t);
    }
    pendingThreads.clear();

    for (int i = (int)threads.size() - 1; i >= 0; i--) {
        ScriptThread& thread = threads[i];

        if (thread.state == ScriptThread::DONE) {
            threads.erase(threads.begin() + i);
            continue;
        }

        // Handle wait states
        if (thread.state == ScriptThread::WAITING_SECS) {
            thread.waitTimer -= dt;
            if (thread.waitTimer <= 0.0) {
                thread.state = ScriptThread::RUNNING;
            } else continue;
        }

        if (thread.state == ScriptThread::WAITING_SOUND) {
            if (!AudioManager::getInstance().isPlaying()) {
                thread.state = ScriptThread::RUNNING;
            } else continue;
        }

        // Execute up to MAX_STEPS_PER_FRAME blocks
        thread.stepsThisFrame = 0;
        executeThread(thread, dt);
    }
}

// -----------------------------------------------------------------------
// Execute a single thread for one frame
// -----------------------------------------------------------------------
void ScratchVM::executeThread(ScriptThread& thread, double dt) {
    while (!thread.currentBlockId.empty()
           && thread.state == ScriptThread::RUNNING
           && thread.stepsThisFrame < MAX_STEPS_PER_FRAME)
    {
        bool yielded = false;
        ScratchValue result = executeBlock(thread, thread.currentBlockId, yielded);
        thread.stepsThisFrame++;

        if (yielded) break; // yield until next frame

        // Advance to next block in sequence
        ScratchBlock* b = getBlock(thread, thread.currentBlockId);
        if (b && !b->nextId.empty()) {
            thread.currentBlockId = b->nextId;
        } else {
            // End of sequence — pop call stack or finish
            if (!thread.callStack.empty()) {
                auto frame = thread.callStack.back();
                thread.callStack.pop_back();
                thread.currentBlockId = frame.blockId;
            } else {
                thread.state = ScriptThread::DONE;
                break;
            }
        }
    }
}

// -----------------------------------------------------------------------
// Execute a single block
// Returns value for reporters; sets yielded=true to pause this frame
// -----------------------------------------------------------------------
ScratchValue ScratchVM::executeBlock(ScriptThread& thread,
                                      const std::string& blockId,
                                      bool& yielded) {
    yielded = false;
    ScratchBlock* b = getBlock(thread, blockId);
    if (!b) { thread.state = ScriptThread::DONE; return ScratchValue(); }

    ScratchValue out;

    switch (b->opcode) {
        // --- Motion ---
        case BlockOpcode::MOTION_MOVESTEPS: {
            double steps = evaluateInput(thread, b->inputs.at("STEPS")).toNum();
            double rad = (thread.sprite->direction - 90.0) * M_PI / 180.0;
            thread.sprite->x += steps * cos(rad);
            thread.sprite->y -= steps * sin(rad);
            break;
        }
        case BlockOpcode::MOTION_TURNRIGHT: {
            double deg = evaluateInput(thread, b->inputs.at("DEGREES")).toNum();
            thread.sprite->direction = ((int)(thread.sprite->direction + deg) % 360 + 360) % 360;
            break;
        }
        case BlockOpcode::MOTION_TURNLEFT: {
            double deg = evaluateInput(thread, b->inputs.at("DEGREES")).toNum();
            thread.sprite->direction = ((int)(thread.sprite->direction - deg) % 360 + 360) % 360;
            break;
        }
        case BlockOpcode::MOTION_GOTOXY: {
            thread.sprite->x = evaluateInput(thread, b->inputs.at("X")).toNum();
            thread.sprite->y = evaluateInput(thread, b->inputs.at("Y")).toNum();
            break;
        }
        case BlockOpcode::MOTION_SETX:
            thread.sprite->x = evaluateInput(thread, b->inputs.at("X")).toNum();
            break;
        case BlockOpcode::MOTION_SETY:
            thread.sprite->y = evaluateInput(thread, b->inputs.at("Y")).toNum();
            break;
        case BlockOpcode::MOTION_CHANGEXBY:
            thread.sprite->x += evaluateInput(thread, b->inputs.at("DX")).toNum();
            break;
        case BlockOpcode::MOTION_CHANGEYBY:
            thread.sprite->y += evaluateInput(thread, b->inputs.at("DY")).toNum();
            break;
        case BlockOpcode::MOTION_IFONEDGEBOUNCE: {
            ScratchSprite* s = thread.sprite;
            // Stage is 480x360, NDS scale: 240x160 (half)
            if (s->x > 220)  { s->x =  220; s->direction = 360 - s->direction; }
            if (s->x < -220) { s->x = -220; s->direction = 360 - s->direction; }
            if (s->y > 160)  { s->y =  160; s->direction = 180 - s->direction; }
            if (s->y < -160) { s->y = -160; s->direction = 180 - s->direction; }
            break;
        }

        // --- Looks ---
        case BlockOpcode::LOOKS_SAY:
        case BlockOpcode::LOOKS_SAYFORSECS: {
            // Store message in sprite for renderer to display
            if (b->inputs.count("MESSAGE"))
                thread.sprite->sayMessage = evaluateInput(thread, b->inputs.at("MESSAGE")).toStr();
            if (b->opcode == BlockOpcode::LOOKS_SAYFORSECS && b->inputs.count("SECS")) {
                thread.state = ScriptThread::WAITING_SECS;
                thread.waitTimer = evaluateInput(thread, b->inputs.at("SECS")).toNum();
                yielded = true;
            }
            break;
        }
        case BlockOpcode::LOOKS_SWITCHCOSTUMETO: {
            if (b->inputs.count("COSTUME")) {
                ScratchValue cosName = evaluateInput(thread, b->inputs.at("COSTUME"));
                for (int i = 0; i < (int)thread.sprite->costumes.size(); i++) {
                    if (thread.sprite->costumes[i].name == cosName.toStr()) {
                        thread.sprite->currentCostume = i; break;
                    }
                }
            }
            break;
        }
        case BlockOpcode::LOOKS_NEXTCOSTUME:
            thread.sprite->currentCostume =
                (thread.sprite->currentCostume + 1) % thread.sprite->costumes.size();
            break;
        case BlockOpcode::LOOKS_SHOW:   thread.sprite->visible = true;  break;
        case BlockOpcode::LOOKS_HIDE:   thread.sprite->visible = false; break;
        case BlockOpcode::LOOKS_SETSIZETO:
            thread.sprite->size = evaluateInput(thread, b->inputs.at("SIZE")).toNum();
            break;
        case BlockOpcode::LOOKS_CHANGESIZEBY:
            thread.sprite->size += evaluateInput(thread, b->inputs.at("CHANGE")).toNum();
            break;

        // --- Sound ---
        case BlockOpcode::SOUND_PLAY:
        case BlockOpcode::SOUND_PLAYUNTILDONE: {
            if (b->inputs.count("SOUND_MENU")) {
                ScratchValue sndName = evaluateInput(thread, b->inputs.at("SOUND_MENU"));
                AudioManager::getInstance().playSound(thread.sprite, sndName.toStr());
                if (b->opcode == BlockOpcode::SOUND_PLAYUNTILDONE) {
                    thread.state = ScriptThread::WAITING_SOUND;
                    yielded = true;
                }
            }
            break;
        }
        case BlockOpcode::SOUND_STOPALLSOUNDS:
            AudioManager::getInstance().stopAll();
            break;

        // --- Control ---
        case BlockOpcode::CONTROL_WAIT: {
            double secs = evaluateInput(thread, b->inputs.at("DURATION")).toNum();
            thread.state = ScriptThread::WAITING_SECS;
            thread.waitTimer = secs;
            yielded = true;
            break;
        }
        case BlockOpcode::CONTROL_REPEAT: {
            int times = (int)evaluateInput(thread, b->inputs.at("TIMES")).toNum();
            if (times > 0 && b->inputs.count("SUBSTACK")) {
                std::string subId = b->inputs.at("SUBSTACK").blockId;
                // Push frame to return here after substack, decrement counter
                ScriptThread::StackFrame frame;
                frame.blockId = blockId; // return to REPEAT block to re-check
                frame.loopCounter = times - 1;
                thread.callStack.push_back(frame);
                thread.currentBlockId = subId;
                yielded = true; // force re-entry via stack
            }
            break;
        }
        case BlockOpcode::CONTROL_FOREVER: {
            if (b->inputs.count("SUBSTACK")) {
                std::string subId = b->inputs.at("SUBSTACK").blockId;
                ScriptThread::StackFrame frame;
                frame.blockId = blockId;
                frame.loopCounter = -1; // infinite
                thread.callStack.push_back(frame);
                thread.currentBlockId = subId;
                yielded = true;
            }
            break;
        }
        case BlockOpcode::CONTROL_IF: {
            bool cond = false;
            if (b->inputs.count("CONDITION"))
                cond = evaluateInput(thread, b->inputs.at("CONDITION")).toBool();
            if (cond && b->inputs.count("SUBSTACK")) {
                thread.currentBlockId = b->inputs.at("SUBSTACK").blockId;
                yielded = true;
            }
            break;
        }
        case BlockOpcode::CONTROL_IF_ELSE: {
            bool cond = false;
            if (b->inputs.count("CONDITION"))
                cond = evaluateInput(thread, b->inputs.at("CONDITION")).toBool();
            std::string substackKey = cond ? "SUBSTACK" : "SUBSTACK2";
            if (b->inputs.count(substackKey)) {
                thread.currentBlockId = b->inputs.at(substackKey).blockId;
                yielded = true;
            }
            break;
        }
        case BlockOpcode::CONTROL_STOP: {
            auto it = b->fields.find("STOP_OPTION");
            std::string opt = (it != b->fields.end()) ? it->second : "all";
            if (opt == "all") stopAll();
            else if (opt == "this script") { thread.state = ScriptThread::DONE; }
            break;
        }
        case BlockOpcode::CONTROL_WAIT_UNTIL: {
            if (b->inputs.count("CONDITION")) {
                bool cond = evaluateInput(thread, b->inputs.at("CONDITION")).toBool();
                if (!cond) { yielded = true; } // re-check next frame (stay on this block)
            }
            break;
        }

        // --- Operators (reporters) ---
        case BlockOpcode::OPERATOR_ADD:
            out = ScratchValue(
                evaluateInput(thread, b->inputs.at("NUM1")).toNum() +
                evaluateInput(thread, b->inputs.at("NUM2")).toNum());
            break;
        case BlockOpcode::OPERATOR_SUBTRACT:
            out = ScratchValue(
                evaluateInput(thread, b->inputs.at("NUM1")).toNum() -
                evaluateInput(thread, b->inputs.at("NUM2")).toNum());
            break;
        case BlockOpcode::OPERATOR_MULTIPLY:
            out = ScratchValue(
                evaluateInput(thread, b->inputs.at("NUM1")).toNum() *
                evaluateInput(thread, b->inputs.at("NUM2")).toNum());
            break;
        case BlockOpcode::OPERATOR_DIVIDE: {
            double d = evaluateInput(thread, b->inputs.at("NUM2")).toNum();
            out = ScratchValue(d != 0 ?
                evaluateInput(thread, b->inputs.at("NUM1")).toNum() / d : 0.0);
            break;
        }
        case BlockOpcode::OPERATOR_RANDOM: {
            double lo = evaluateInput(thread, b->inputs.at("FROM")).toNum();
            double hi = evaluateInput(thread, b->inputs.at("TO")).toNum();
            if (lo > hi) { double t = lo; lo = hi; hi = t; }
            out = ScratchValue(lo + (double)rand() / RAND_MAX * (hi - lo));
            break;
        }
        case BlockOpcode::OPERATOR_GT:
            out = ScratchValue(evaluateInput(thread, b->inputs.at("OPERAND1"))
                             > evaluateInput(thread, b->inputs.at("OPERAND2")) ? 1.0 : 0.0);
            break;
        case BlockOpcode::OPERATOR_LT:
            out = ScratchValue(evaluateInput(thread, b->inputs.at("OPERAND1"))
                             < evaluateInput(thread, b->inputs.at("OPERAND2")) ? 1.0 : 0.0);
            break;
        case BlockOpcode::OPERATOR_EQUALS:
            out = ScratchValue(evaluateInput(thread, b->inputs.at("OPERAND1"))
                            == evaluateInput(thread, b->inputs.at("OPERAND2")) ? 1.0 : 0.0);
            break;
        case BlockOpcode::OPERATOR_AND:
            out = ScratchValue(
                evaluateInput(thread, b->inputs.at("OPERAND1")).toBool() &&
                evaluateInput(thread, b->inputs.at("OPERAND2")).toBool() ? 1.0 : 0.0);
            break;
        case BlockOpcode::OPERATOR_OR:
            out = ScratchValue(
                evaluateInput(thread, b->inputs.at("OPERAND1")).toBool() ||
                evaluateInput(thread, b->inputs.at("OPERAND2")).toBool() ? 1.0 : 0.0);
            break;
        case BlockOpcode::OPERATOR_NOT:
            out = ScratchValue(!evaluateInput(thread, b->inputs.at("OPERAND")).toBool() ? 1.0 : 0.0);
            break;
        case BlockOpcode::OPERATOR_JOIN: {
            std::string a = evaluateInput(thread, b->inputs.at("STRING1")).toStr();
            std::string bv = evaluateInput(thread, b->inputs.at("STRING2")).toStr();
            out = ScratchValue(a + bv);
            break;
        }
        case BlockOpcode::OPERATOR_MOD: {
            double n = evaluateInput(thread, b->inputs.at("NUM1")).toNum();
            double d = evaluateInput(thread, b->inputs.at("NUM2")).toNum();
            out = ScratchValue(d != 0 ? fmod(n, d) : 0.0);
            break;
        }
        case BlockOpcode::OPERATOR_ROUND:
            out = ScratchValue(round(evaluateInput(thread, b->inputs.at("NUM")).toNum()));
            break;
        case BlockOpcode::OPERATOR_MATHOP: {
            double n = evaluateInput(thread, b->inputs.at("NUM")).toNum();
            auto it = b->fields.find("OPERATOR");
            std::string op = (it != b->fields.end()) ? it->second : "";
            if (op == "abs")   out = ScratchValue(fabs(n));
            else if (op == "floor") out = ScratchValue(floor(n));
            else if (op == "ceiling") out = ScratchValue(ceil(n));
            else if (op == "sqrt")  out = ScratchValue(sqrt(n));
            else if (op == "sin")   out = ScratchValue(sin(n * M_PI / 180.0));
            else if (op == "cos")   out = ScratchValue(cos(n * M_PI / 180.0));
            else if (op == "tan")   out = ScratchValue(tan(n * M_PI / 180.0));
            else if (op == "asin")  out = ScratchValue(asin(n) * 180.0 / M_PI);
            else if (op == "acos")  out = ScratchValue(acos(n) * 180.0 / M_PI);
            else if (op == "atan")  out = ScratchValue(atan(n) * 180.0 / M_PI);
            else if (op == "ln")    out = ScratchValue(log(n));
            else if (op == "log")   out = ScratchValue(log10(n));
            else if (op == "e ^")   out = ScratchValue(exp(n));
            else if (op == "10 ^")  out = ScratchValue(pow(10.0, n));
            break;
        }

        // --- Sensing ---
        case BlockOpcode::SENSING_TIMER:
            out = ScratchValue(globalTimer);
            break;
        case BlockOpcode::SENSING_RESETTIMER:
            globalTimer = 0.0;
            break;
        case BlockOpcode::SENSING_MOUSEX:
            out = ScratchValue((double)InputHandler::getInstance().getTouchX());
            break;
        case BlockOpcode::SENSING_MOUSEY:
            out = ScratchValue((double)InputHandler::getInstance().getTouchY());
            break;
        case BlockOpcode::SENSING_MOUSEDOWN:
            out = ScratchValue(InputHandler::getInstance().isTouching() ? 1.0 : 0.0);
            break;
        case BlockOpcode::SENSING_LOUDNESS:
        case BlockOpcode::NDS_MICROPHONE_LOUDNESS:
            out = ScratchValue((double)InputHandler::getInstance().getMicLoudness());
            break;
        case BlockOpcode::SENSING_KEYPRESSED: {
            auto it = b->fields.find("KEY_OPTION");
            std::string key = (it != b->fields.end()) ? it->second : "";
            out = ScratchValue(InputHandler::getInstance().isKeyDown(key) ? 1.0 : 0.0);
            break;
        }

        // --- Data ---
        case BlockOpcode::DATA_SETVARIABLETO: {
            auto it = b->fields.find("VARIABLE");
            if (it != b->fields.end() && b->inputs.count("VALUE")) {
                ScratchValue val = evaluateInput(thread, b->inputs.at("VALUE"));
                setVariable(thread.sprite, it->second, val);
            }
            break;
        }
        case BlockOpcode::DATA_CHANGEVARIABLEBY: {
            auto it = b->fields.find("VARIABLE");
            if (it != b->fields.end() && b->inputs.count("VALUE")) {
                ScratchValue delta = evaluateInput(thread, b->inputs.at("VALUE"));
                ScratchValue cur = getVariable(thread.sprite, it->second);
                setVariable(thread.sprite, it->second,
                    ScratchValue(cur.toNum() + delta.toNum()));
            }
            break;
        }

        // --- NDS Extension ---
        case BlockOpcode::NDS_BUTTONPRESSED: {
            auto it = b->fields.find("BUTTON");
            std::string btn = (it != b->fields.end()) ? it->second : "";
            out = ScratchValue(InputHandler::getInstance().isButtonDown(btn) ? 1.0 : 0.0);
            break;
        }
        case BlockOpcode::NDS_BUTTONHELD: {
            auto it = b->fields.find("BUTTON");
            std::string btn = (it != b->fields.end()) ? it->second : "";
            out = ScratchValue(InputHandler::getInstance().isButtonHeld(btn) ? 1.0 : 0.0);
            break;
        }
        case BlockOpcode::NDS_BUTTONRELEASED: {
            auto it = b->fields.find("BUTTON");
            std::string btn = (it != b->fields.end()) ? it->second : "";
            out = ScratchValue(InputHandler::getInstance().isButtonReleased(btn) ? 1.0 : 0.0);
            break;
        }
        case BlockOpcode::NDS_TOUCHX:
            out = ScratchValue((double)InputHandler::getInstance().getTouchX());
            break;
        case BlockOpcode::NDS_TOUCHY:
            out = ScratchValue((double)InputHandler::getInstance().getTouchY());
            break;
        case BlockOpcode::NDS_TOUCHPRESSED:
            out = ScratchValue(InputHandler::getInstance().isTouching() ? 1.0 : 0.0);
            break;
        case BlockOpcode::NDS_RUMBLE:
            // Rumble pak support via GBA slot
            // GBA_BUS[0] = 0x0002; // Enable rumble (slot-2 device)
            break;

        default:
            break;
    }

    return out;
}

// -----------------------------------------------------------------------
// Evaluate an input slot (literal or reporter block)
// -----------------------------------------------------------------------
ScratchValue ScratchVM::evaluateInput(ScriptThread& thread,
                                       const ScratchInput& input) {
    if (!input.blockId.empty()) {
        return evaluateReporter(thread, input.blockId);
    }
    // Literal value
    if (input.valueType >= 4 && input.valueType <= 8) { // numeric types
        return ScratchValue(input.numValue);
    }
    return ScratchValue(input.strValue);
}

// -----------------------------------------------------------------------
// Evaluate a reporter block (returns a value, no side effects)
// -----------------------------------------------------------------------
ScratchValue ScratchVM::evaluateReporter(ScriptThread& thread,
                                          const std::string& blockId) {
    bool yielded = false;
    return executeBlock(thread, blockId, yielded);
}

// -----------------------------------------------------------------------
// Broadcast
// -----------------------------------------------------------------------
void ScratchVM::broadcast(const std::string& name) {
    for (auto& sprite : project->targets) {
        startHatBlocks(BlockOpcode::EVENT_WHENBROADCASTRECEIVED, &sprite, name);
    }
}

// -----------------------------------------------------------------------
// Stop all scripts
// -----------------------------------------------------------------------
void ScratchVM::stopAll() {
    for (auto& t : threads) t.state = ScriptThread::DONE;
    pendingThreads.clear();
    AudioManager::getInstance().stopAll();
}

// -----------------------------------------------------------------------
// Variable accessors — check sprite-local first, then stage globals
// -----------------------------------------------------------------------
ScratchValue ScratchVM::getVariable(ScratchSprite* sprite,
                                     const std::string& name) {
    if (sprite) {
        for (auto& kv : sprite->variables) {
            if (kv.second.name == name) return ScratchValue(kv.second.value);
        }
    }
    // Check stage globals
    if (project->getStage()) {
        for (auto& kv : project->getStage()->variables) {
            if (kv.second.name == name) return ScratchValue(kv.second.value);
        }
    }
    return ScratchValue(0.0);
}

void ScratchVM::setVariable(ScratchSprite* sprite,
                             const std::string& name, ScratchValue val) {
    if (sprite) {
        for (auto& kv : sprite->variables) {
            if (kv.second.name == name) { kv.second.value = val.toStr(); return; }
        }
    }
    if (project->getStage()) {
        for (auto& kv : project->getStage()->variables) {
            if (kv.second.name == name) { kv.second.value = val.toStr(); return; }
        }
    }
}

// -----------------------------------------------------------------------
// Lookup block in sprite's block map
// -----------------------------------------------------------------------
ScratchBlock* ScratchVM::getBlock(ScriptThread& t, const std::string& id) {
    for (auto& b : t.sprite->blocks) {
        if (b.id == id) return &b;
    }
    return nullptr;
}

// -----------------------------------------------------------------------
// Input event firing
// -----------------------------------------------------------------------
void ScratchVM::fireSpriteClicked(ScratchSprite* sprite) {
    startHatBlocks(BlockOpcode::EVENT_WHENTHISSPRITECLICKED, sprite);
}
void ScratchVM::fireKeyPressed(const std::string& key) {
    for (auto& sprite : project->targets) {
        startHatBlocks(BlockOpcode::EVENT_WHENKEYPRESSED, &sprite, key);
    }
}
