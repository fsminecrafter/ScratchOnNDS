#define _USE_MATH_DEFINES
// =============================================================================
// vm.cpp — Scratch VM execution engine
// Fixes: TURNRIGHT/TURNLEFT were swapped; IFONEDGEBOUNCE caused 1-frame flash.
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
    if (type == STR && o.type == STR) return strVal == o.strVal;
    return toNum() == o.toNum();
}
bool ScratchValue::operator<(const ScratchValue& o) const { return toNum() < o.toNum(); }
bool ScratchValue::operator>(const ScratchValue& o) const { return toNum() > o.toNum(); }

// Normalize Scratch direction into (-180, 180] range
static double normDir(double d) {
    d = fmod(d, 360.0);
    if (d > 180.0)  d -= 360.0;
    if (d <= -180.0) d += 360.0;
    return d;
}

// -----------------------------------------------------------------------
// Init
// -----------------------------------------------------------------------
void ScratchVM::init(ScratchProject& proj) {
    project = &proj;
    globalTimer = 0.0;
    threads.clear();
}

// -----------------------------------------------------------------------
// Green flag
// -----------------------------------------------------------------------
void ScratchVM::greenFlag() {
    threads.clear();
    globalTimer = 0.0;
    for (auto& sprite : project->targets)
        startHatBlocks(BlockOpcode::EVENT_WHENFLAGCLICKED, &sprite);
}

// -----------------------------------------------------------------------
// Start threads for a hat block type on a sprite
// -----------------------------------------------------------------------
void ScratchVM::startHatBlocks(BlockOpcode hat, ScratchSprite* sprite,
                                const std::string& field) {
    for (auto& b : sprite->blocks) {
        if (!b.topLevel) continue;
        if (b.opcode != hat) continue;

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
        thread.currentBlockId = b.nextId;
        thread.state = ScriptThread::RUNNING;
        thread.waitTimer = 0.0;
        thread.isClone = false;
        thread.stepsThisFrame = 0;
        if (!thread.currentBlockId.empty())
            threads.push_back(thread);
    }
}

// -----------------------------------------------------------------------
// Step
// -----------------------------------------------------------------------
void ScratchVM::step(double dt) {
    globalTimer += dt;

    for (auto& t : pendingThreads) {
        if (threads.size() < 64)
            threads.push_back(t);
    }
    pendingThreads.clear();

    for (int i = (int)threads.size() - 1; i >= 0; i--) {
        ScriptThread& thread = threads[i];

        if (thread.state == ScriptThread::DONE) {
            threads.erase(threads.begin() + i);
            continue;
        }
        if (thread.state == ScriptThread::WAITING_SECS) {
            thread.waitTimer -= dt;
            if (thread.waitTimer <= 0.0) thread.state = ScriptThread::RUNNING;
            else continue;
        }
        if (thread.state == ScriptThread::WAITING_SOUND) {
            if (!AudioManager::getInstance().isPlaying()) thread.state = ScriptThread::RUNNING;
            else continue;
        }

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
        executeBlock(thread, thread.currentBlockId, yielded);
        thread.stepsThisFrame++;

        if (yielded) break;

        ScratchBlock* b = getBlock(thread, thread.currentBlockId);

        if (b && !b->nextId.empty()) {
            thread.currentBlockId = b->nextId;
        } else {
            if (!thread.callStack.empty()) {
                auto& frame = thread.callStack.back();
                if (frame.remaining > 0) frame.remaining--;
                if (frame.remaining == 0) {
                    thread.callStack.pop_back();
                    if (!thread.callStack.empty())
                        thread.currentBlockId = thread.callStack.back().returnBlockId;
                    else
                        thread.state = ScriptThread::DONE;
                } else {
                    thread.currentBlockId = frame.returnBlockId;
                }
                continue;
            }
            thread.state = ScriptThread::DONE;
            break;
        }
    }
}

// -----------------------------------------------------------------------
// Execute a single block
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
            thread.sprite->x += cos(rad) * steps;
            thread.sprite->y -= sin(rad) * steps; // NDS Y is inverted vs Scratch
            if (!std::isfinite(thread.sprite->x)) thread.sprite->x = 0;
            if (!std::isfinite(thread.sprite->y)) thread.sprite->y = 0;
            break;
        }

        // FIX: TURNRIGHT increases direction (clockwise), TURNLEFT decreases it.
        // Previously these were swapped.
        case BlockOpcode::MOTION_TURNRIGHT: {
            double deg = evaluateInput(thread, b->inputs.at("DEGREES")).toNum();
            thread.sprite->direction = normDir(thread.sprite->direction + deg);
            break;
        }
        case BlockOpcode::MOTION_TURNLEFT: {
            double deg = evaluateInput(thread, b->inputs.at("DEGREES")).toNum();
            thread.sprite->direction = normDir(thread.sprite->direction - deg);
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

        // FIX: Edge bounce — clamp position first, then reflect the relevant
        // velocity component. The old code used `360 - dir` which could produce
        // a direction that immediately satisfied the other wall condition,
        // causing a 1-frame position flash to 0,0.
        case BlockOpcode::MOTION_IFONEDGEBOUNCE: {
            ScratchSprite* s = thread.sprite;
            const double XMAX = 220.0, YMAX = 160.0;
            bool hitX = false, hitY = false;

            if (s->x > XMAX)  { s->x =  XMAX; hitX = true; }
            if (s->x < -XMAX) { s->x = -XMAX; hitX = true; }
            if (s->y > YMAX)  { s->y =  YMAX; hitY = true; }
            if (s->y < -YMAX) { s->y = -YMAX; hitY = true; }

            if (hitX || hitY) {
                // Convert Scratch direction to standard math angle
                double rad = (s->direction - 90.0) * M_PI / 180.0;
                double dx = cos(rad);
                double dy = -sin(rad);
                if (hitX) dx = -dx;
                if (hitY) dy = -dy;
                // Convert back to Scratch direction
                double newDir = atan2(-dy, dx) * 180.0 / M_PI + 90.0;
                s->direction = normDir(newDir);
            }
            break;
        }

        // --- Looks ---
        case BlockOpcode::LOOKS_SAY:
        case BlockOpcode::LOOKS_SAYFORSECS: {
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
            if (!thread.sprite->costumes.empty())
                thread.sprite->currentCostume =
                    (thread.sprite->currentCostume + 1) % (int)thread.sprite->costumes.size();
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
            if (times <= 0 || !b->inputs.count("SUBSTACK")) break;
            std::string subId = b->inputs.at("SUBSTACK").blockId;
            thread.callStack.push_back({ blockId, blockId, times });
            thread.currentBlockId = subId;
            yielded = true;
            break;
        }
        case BlockOpcode::CONTROL_FOREVER: {
            if (!b->inputs.count("SUBSTACK")) break;
            std::string subId = b->inputs.at("SUBSTACK").blockId;
            thread.callStack.push_back({ blockId, blockId, -1 });
            thread.currentBlockId = subId;
            yielded = true;
            break;
        }
        case BlockOpcode::CONTROL_IF: {
            bool cond = b->inputs.count("CONDITION") &&
                        evaluateInput(thread, b->inputs.at("CONDITION")).toBool();
            if (cond && b->inputs.count("SUBSTACK")) {
                thread.currentBlockId = b->inputs.at("SUBSTACK").blockId;
                yielded = true;
            }
            break;
        }
        case BlockOpcode::CONTROL_IF_ELSE: {
            bool cond = b->inputs.count("CONDITION") &&
                        evaluateInput(thread, b->inputs.at("CONDITION")).toBool();
            const char* key = cond ? "SUBSTACK" : "SUBSTACK2";
            if (b->inputs.count(key)) {
                thread.currentBlockId = b->inputs.at(key).blockId;
                yielded = true;
            }
            break;
        }
        case BlockOpcode::CONTROL_STOP: {
            auto it = b->fields.find("STOP_OPTION");
            std::string opt = (it != b->fields.end()) ? it->second : "all";
            if (opt == "all") stopAll();
            else if (opt == "this script") thread.state = ScriptThread::DONE;
            break;
        }
        case BlockOpcode::CONTROL_WAIT_UNTIL: {
            if (b->inputs.count("CONDITION") &&
                !evaluateInput(thread, b->inputs.at("CONDITION")).toBool())
                yielded = true;
            break;
        }

        // --- Operators ---
        case BlockOpcode::OPERATOR_ADD:
            out = ScratchValue(evaluateInput(thread, b->inputs.at("NUM1")).toNum() +
                               evaluateInput(thread, b->inputs.at("NUM2")).toNum());
            break;
        case BlockOpcode::OPERATOR_SUBTRACT:
            out = ScratchValue(evaluateInput(thread, b->inputs.at("NUM1")).toNum() -
                               evaluateInput(thread, b->inputs.at("NUM2")).toNum());
            break;
        case BlockOpcode::OPERATOR_MULTIPLY:
            out = ScratchValue(evaluateInput(thread, b->inputs.at("NUM1")).toNum() *
                               evaluateInput(thread, b->inputs.at("NUM2")).toNum());
            break;
        case BlockOpcode::OPERATOR_DIVIDE: {
            double d = evaluateInput(thread, b->inputs.at("NUM2")).toNum();
            out = ScratchValue(d != 0.0 ? evaluateInput(thread, b->inputs.at("NUM1")).toNum() / d : 0.0);
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
            out = ScratchValue(evaluateInput(thread, b->inputs.at("OPERAND1")) >
                               evaluateInput(thread, b->inputs.at("OPERAND2")) ? 1.0 : 0.0);
            break;
        case BlockOpcode::OPERATOR_LT:
            out = ScratchValue(evaluateInput(thread, b->inputs.at("OPERAND1")) <
                               evaluateInput(thread, b->inputs.at("OPERAND2")) ? 1.0 : 0.0);
            break;
        case BlockOpcode::OPERATOR_EQUALS:
            out = ScratchValue(evaluateInput(thread, b->inputs.at("OPERAND1")) ==
                               evaluateInput(thread, b->inputs.at("OPERAND2")) ? 1.0 : 0.0);
            break;
        case BlockOpcode::OPERATOR_AND:
            out = ScratchValue(evaluateInput(thread, b->inputs.at("OPERAND1")).toBool() &&
                               evaluateInput(thread, b->inputs.at("OPERAND2")).toBool() ? 1.0 : 0.0);
            break;
        case BlockOpcode::OPERATOR_OR:
            out = ScratchValue(evaluateInput(thread, b->inputs.at("OPERAND1")).toBool() ||
                               evaluateInput(thread, b->inputs.at("OPERAND2")).toBool() ? 1.0 : 0.0);
            break;
        case BlockOpcode::OPERATOR_NOT:
            out = ScratchValue(!evaluateInput(thread, b->inputs.at("OPERAND")).toBool() ? 1.0 : 0.0);
            break;
        case BlockOpcode::OPERATOR_JOIN: {
            std::string a  = evaluateInput(thread, b->inputs.at("STRING1")).toStr();
            std::string bv = evaluateInput(thread, b->inputs.at("STRING2")).toStr();
            out = ScratchValue(a + bv);
            break;
        }
        case BlockOpcode::OPERATOR_MOD: {
            double n = evaluateInput(thread, b->inputs.at("NUM1")).toNum();
            double d = evaluateInput(thread, b->inputs.at("NUM2")).toNum();
            out = ScratchValue(d != 0.0 ? fmod(n, d) : 0.0);
            break;
        }
        case BlockOpcode::OPERATOR_ROUND:
            out = ScratchValue(round(evaluateInput(thread, b->inputs.at("NUM")).toNum()));
            break;
        case BlockOpcode::OPERATOR_MATHOP: {
            double n = evaluateInput(thread, b->inputs.at("NUM")).toNum();
            auto it = b->fields.find("OPERATOR");
            std::string op = (it != b->fields.end()) ? it->second : "";
            if      (op == "abs")     out = ScratchValue(fabs(n));
            else if (op == "floor")   out = ScratchValue(floor(n));
            else if (op == "ceiling") out = ScratchValue(ceil(n));
            else if (op == "sqrt")    out = ScratchValue(sqrt(n < 0.0 ? 0.0 : n));
            else if (op == "sin")     out = ScratchValue(sin(n * M_PI / 180.0));
            else if (op == "cos")     out = ScratchValue(cos(n * M_PI / 180.0));
            else if (op == "tan")     out = ScratchValue(tan(n * M_PI / 180.0));
            else if (op == "asin")    out = ScratchValue(asin(n) * 180.0 / M_PI);
            else if (op == "acos")    out = ScratchValue(acos(n) * 180.0 / M_PI);
            else if (op == "atan")    out = ScratchValue(atan(n) * 180.0 / M_PI);
            else if (op == "ln")      out = ScratchValue(n > 0.0 ? log(n) : 0.0);
            else if (op == "log")     out = ScratchValue(n > 0.0 ? log10(n) : 0.0);
            else if (op == "e ^")     out = ScratchValue(exp(n));
            else if (op == "10 ^")    out = ScratchValue(pow(10.0, n));
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
            if (it != b->fields.end() && b->inputs.count("VALUE"))
                setVariable(thread.sprite, it->second,
                            evaluateInput(thread, b->inputs.at("VALUE")));
            break;
        }
        case BlockOpcode::DATA_CHANGEVARIABLEBY: {
            auto it = b->fields.find("VARIABLE");
            if (it != b->fields.end() && b->inputs.count("VALUE")) {
                ScratchValue delta = evaluateInput(thread, b->inputs.at("VALUE"));
                ScratchValue cur   = getVariable(thread.sprite, it->second);
                setVariable(thread.sprite, it->second,
                            ScratchValue(cur.toNum() + delta.toNum()));
            }
            break;
        }

        // --- NDS Extension ---
        case BlockOpcode::NDS_BUTTONPRESSED: {
            auto it = b->fields.find("BUTTON");
            out = ScratchValue(InputHandler::getInstance().isButtonDown(
                it != b->fields.end() ? it->second : "") ? 1.0 : 0.0);
            break;
        }
        case BlockOpcode::NDS_BUTTONHELD: {
            auto it = b->fields.find("BUTTON");
            out = ScratchValue(InputHandler::getInstance().isButtonHeld(
                it != b->fields.end() ? it->second : "") ? 1.0 : 0.0);
            break;
        }
        case BlockOpcode::NDS_BUTTONRELEASED: {
            auto it = b->fields.find("BUTTON");
            out = ScratchValue(InputHandler::getInstance().isButtonReleased(
                it != b->fields.end() ? it->second : "") ? 1.0 : 0.0);
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
            break;

        default:
            break;
    }

    return out;
}

// -----------------------------------------------------------------------
// Evaluate an input slot
// -----------------------------------------------------------------------
ScratchValue ScratchVM::evaluateInput(ScriptThread& thread, const ScratchInput& input) {
    if (!input.blockId.empty())
        return evaluateReporter(thread, input.blockId);
    if (input.valueType >= 4 && input.valueType <= 8)
        return ScratchValue(input.numValue);
    return ScratchValue(input.strValue);
}

ScratchValue ScratchVM::evaluateReporter(ScriptThread& thread, const std::string& blockId) {
    bool yielded = false;
    ScratchValue v = executeBlock(thread, blockId, yielded);
    thread.state = ScriptThread::RUNNING;
    return v;
}

// -----------------------------------------------------------------------
// Broadcast / stop
// -----------------------------------------------------------------------
void ScratchVM::broadcast(const std::string& name) {
    for (auto& sprite : project->targets)
        startHatBlocks(BlockOpcode::EVENT_WHENBROADCASTRECEIVED, &sprite, name);
}

void ScratchVM::stopAll() {
    for (auto& t : threads) t.state = ScriptThread::DONE;
    pendingThreads.clear();
    AudioManager::getInstance().stopAll();
}

// -----------------------------------------------------------------------
// Variable accessors
// -----------------------------------------------------------------------
ScratchValue ScratchVM::getVariable(ScratchSprite* sprite, const std::string& name) {
    if (sprite)
        for (auto& var : sprite->variables)
            if (var.name == name) return ScratchValue(var.value);
    if (project->getStage())
        for (auto& var : project->getStage()->variables)
            if (var.name == name) return ScratchValue(var.value);
    return ScratchValue(0.0);
}

void ScratchVM::setVariable(ScratchSprite* sprite, const std::string& name, ScratchValue val) {
    if (sprite)
        for (auto& var : sprite->variables)
            if (var.name == name) { var.value = val.toStr(); return; }
    if (project->getStage())
        for (auto& var : project->getStage()->variables)
            if (var.name == name) { var.value = val.toStr(); return; }
}

ScratchBlock* ScratchVM::getBlock(ScriptThread& t, const std::string& id) {
    for (auto& b : t.sprite->blocks)
        if (b.id == id) return &b;
    return nullptr;
}

void ScratchVM::fireSpriteClicked(ScratchSprite* sprite) {
    startHatBlocks(BlockOpcode::EVENT_WHENTHISSPRITECLICKED, sprite);
}
void ScratchVM::fireKeyPressed(const std::string& key) {
    for (auto& sprite : project->targets)
        startHatBlocks(BlockOpcode::EVENT_WHENKEYPRESSED, &sprite, key);
}