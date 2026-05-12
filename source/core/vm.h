// =============================================================================
// vm.h / vm.cpp — Scratch 3.0 Virtual Machine for NDS
// Cooperative coroutine-style execution (no OS threads on NDS)
// =============================================================================
#pragma once
#include "project.h"
#include <vector>
#include <string>
#include <map>
#include <functional>

const std::vector<ScriptThread>& getThreads() const { return threads; }

// -----------------------------------------------------------------------
// Runtime value (Scratch is dynamically typed: number or string)
// -----------------------------------------------------------------------
struct ScratchValue {
    enum Type { NUM, STR } type;
    double numVal;
    std::string strVal;

    ScratchValue() : type(NUM), numVal(0) {}
    ScratchValue(double d) : type(NUM), numVal(d) {}
    ScratchValue(const std::string& s) : type(STR), numVal(0), strVal(s) {}
    ScratchValue(const char* s) : type(STR), numVal(0), strVal(s) {}

    double toNum() const;
    std::string toStr() const;
    bool toBool() const { return toNum() != 0.0; }

    bool operator==(const ScratchValue& o) const;
    bool operator<(const ScratchValue& o) const;
    bool operator>(const ScratchValue& o) const;
};

// -----------------------------------------------------------------------
// Execution thread (one per script/hat block)
// -----------------------------------------------------------------------
struct ScriptThread {
    enum State { RUNNING, WAITING_SECS, WAITING_UNTIL, WAITING_SOUND, DONE };

    ScratchSprite* sprite;
    std::string    currentBlockId;
    State          state;
    double         waitTimer;          // seconds remaining for WAIT
    bool           isClone;

    // Execution stack for nested control structures
    struct StackFrame {
        std::string loopBlockId;
        std::string returnBlockId;
        int remaining = -1; // -1 = forever
    };
    std::vector<StackFrame> callStack;
    int stepsThisFrame;
};

// -----------------------------------------------------------------------
// The VM singleton
// -----------------------------------------------------------------------
class ScratchVM {
public:
    static ScratchVM& getInstance() {
        static ScratchVM inst;
        return inst;
    }

    ScratchVM() {}
    explicit ScratchVM(ScratchProject& proj) { init(proj); }

    void init(ScratchProject& proj);
    void greenFlag();
    void step(double dt);   // call once per frame with delta time
    void broadcast(const std::string& name);
    void stopAll();
    void stopThisScript(ScriptThread* thread);

    // Called by input handler to fire events
    void fireSpriteClicked(ScratchSprite* sprite);
    void fireKeyPressed(const std::string& key);

    // Variable access (used by renderer for monitors)
    ScratchValue getVariable(ScratchSprite* sprite, const std::string& name);
    void setVariable(ScratchSprite* sprite, const std::string& name, ScratchValue val);

    // Global timer (seconds since green flag)
    double globalTimer;

private:
    ScratchProject* project;
    std::vector<ScriptThread> threads;
    std::vector<ScriptThread> pendingThreads; // added during step

    void startHatBlocks(BlockOpcode hat, ScratchSprite* sprite,
                        const std::string& field = "");
    void executeThread(ScriptThread& thread, double dt);
    ScratchValue executeBlock(ScriptThread& thread,
                              const std::string& blockId,
                              bool& yielded);
    ScratchValue evaluateInput(ScriptThread& thread,
                               const ScratchInput& input);
    ScratchValue evaluateReporter(ScriptThread& thread,
                                  const std::string& blockId);

    // Opcode handlers
    bool execMotion(ScriptThread& t, ScratchBlock& b, bool& yielded);
    bool execLooks(ScriptThread& t, ScratchBlock& b, bool& yielded);
    bool execSound(ScriptThread& t, ScratchBlock& b, bool& yielded);
    bool execControl(ScriptThread& t, ScratchBlock& b, bool& yielded);
    bool execSensing(ScriptThread& t, ScratchBlock& b, bool& yielded);
    bool execData(ScriptThread& t, ScratchBlock& b, bool& yielded);
    bool execOperator(ScriptThread& t, ScratchBlock& b, ScratchValue& out);
    bool execNDS(ScriptThread& t, ScratchBlock& b, bool& yielded,
                 ScratchValue& out);

    ScratchBlock* getBlock(ScriptThread& t, const std::string& id);
    static constexpr int MAX_STEPS_PER_FRAME = 1000;
};
