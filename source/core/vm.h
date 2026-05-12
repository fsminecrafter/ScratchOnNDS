// =============================================================================
// vm.h / vm.cpp — Scratch 3.0 Virtual Machine for NDS
// Cooperative coroutine-style execution (no OS threads on NDS)
//
// Extended Scratch 3.0 support:
//   - Lists (add, delete, insert, replace, item, length, contains)
//   - Clones (create, start-as-clone, delete)
//   - Broadcasts (send and wait)
//   - Glide-to with interpolation
//   - Pen (basic: stamp not implemented, but state tracked)
//   - String operators (letter-of, length, contains)
//   - All math ops
//   - sensing_of (get property of another sprite)
//   - Sprite touching sprite / touching edge
//   - Variable monitors (show/hide)
// =============================================================================
#pragma once
#include "project.h"
#include <vector>
#include <string>
#include <map>
#include <functional>

// -----------------------------------------------------------------------
// Forward declarations
// -----------------------------------------------------------------------
class ScratchVM;

// -----------------------------------------------------------------------
// Runtime value (Scratch is dynamically typed: number or string)
// -----------------------------------------------------------------------
struct ScratchValue {
    enum Type : uint8_t { NUM, STR } type;
    double      numVal;
    std::string strVal;

    ScratchValue()                   : type(NUM), numVal(0.0) {}
    explicit ScratchValue(double d)  : type(NUM), numVal(d)   {}
    ScratchValue(const std::string& s) : type(STR), numVal(0.0), strVal(s) {}
    ScratchValue(const char* s)      : type(STR), numVal(0.0), strVal(s)   {}

    double      toNum()  const;
    std::string toStr()  const;
    bool        toBool() const { return toNum() != 0.0; }

    bool operator==(const ScratchValue& o) const;
    bool operator<(const ScratchValue& o)  const;
    bool operator>(const ScratchValue& o)  const;
};

// -----------------------------------------------------------------------
// Runtime list (Scratch lists; stored per-sprite)
// -----------------------------------------------------------------------
struct ScratchRuntimeList {
    std::string              name;
    std::vector<std::string> items;  // all items stored as strings
};

// -----------------------------------------------------------------------
// Call-stack frame for control structures
// -----------------------------------------------------------------------
struct StackFrame {
    std::string loopBlockId;    // the loop/repeat block id (for return)
    std::string returnBlockId;  // block to jump back to on each iteration
    int         remaining;      // -1 = forever, 0 = done, >0 = count
    // For glide
    double      glideStartX, glideStartY;
    double      glideEndX,   glideEndY;
    double      glideDuration, glideElapsed;
    bool        isGlide;
    // For wait-until
    bool        isWaitUntil;
    std::string condBlockId;
    // For broadcast-and-wait
    bool        isBroadcastWait;
    std::string broadcastName;

    StackFrame() : remaining(-1),
                   glideStartX(0), glideStartY(0),
                   glideEndX(0), glideEndY(0),
                   glideDuration(0), glideElapsed(0),
                   isGlide(false), isWaitUntil(false),
                   isBroadcastWait(false) {}
};

// -----------------------------------------------------------------------
// Execution thread (one per script/hat block)
// -----------------------------------------------------------------------
struct ScriptThread {
    enum State : uint8_t {
        RUNNING,
        WAITING_SECS,
        WAITING_UNTIL,
        WAITING_SOUND,
        WAITING_BROADCAST,
        DONE
    };

    ScratchSprite* sprite;
    std::string    currentBlockId;
    State          state;
    double         waitTimer;        // seconds remaining for WAIT
    bool           isClone;
    int            stepsThisFrame;

    // Per-thread local variables (for "for each" future use; currently unused)
    // We keep this lean — no map allocation unless needed.

    std::vector<StackFrame> callStack;

    ScriptThread() : sprite(nullptr), state(RUNNING),
                     waitTimer(0.0), isClone(false), stepsThisFrame(0) {}
};

// -----------------------------------------------------------------------
// Broadcast record — tracks in-flight broadcasts
// -----------------------------------------------------------------------
struct BroadcastRecord {
    std::string name;
    int         threadsLaunched;   // how many threads were started
    int         threadsDone;       // incremented when each finishes
    bool        isWaiting;         // someone is waiting on this
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

    ScratchVM() : answerStr(""), globalTimer(0.0), project(nullptr) {}
    explicit ScratchVM(ScratchProject& proj) { init(proj); }

    void init(ScratchProject& proj);
    void greenFlag();
    void step(double dt);        // call once per frame with delta time in seconds
    void broadcast(const std::string& name);
    void broadcastAndWait(ScriptThread& caller, const std::string& name);
    void stopAll();

    // Called by input handler to fire events
    void fireSpriteClicked(ScratchSprite* sprite);
    void fireKeyPressed(const std::string& key);

    // Variable access (used by renderer for monitors)
    ScratchValue getVariable(ScratchSprite* sprite, const std::string& name);
    void         setVariable(ScratchSprite* sprite, const std::string& name,
                              const ScratchValue& val);

    // List access
    ScratchRuntimeList* getList(ScratchSprite* sprite, const std::string& name);

    // Clone management
    ScratchSprite* createClone(ScratchSprite* parent);
    void           deleteClone(ScratchSprite* sprite);

    // "ask and wait" answer
    std::string answerStr;

    // Global timer (seconds since green flag)
    double globalTimer;

    // Thread accessor for overlay/debug
    const std::vector<ScriptThread>& getThreads() const { return threads; }

private:
    ScratchProject* project;

    std::vector<ScriptThread>  threads;
    std::vector<ScriptThread>  pendingThreads;  // spawned during step(), added next frame
    std::vector<BroadcastRecord> broadcasts;

    // Per-sprite runtime lists (keyed by sprite ptr + list name)
    // Stored flat to avoid map overhead on NDS
    struct ListEntry {
        ScratchSprite*     owner;   // nullptr = global
        ScratchRuntimeList list;
    };
    std::vector<ListEntry> runtimeLists;

    // Clone sprites (allocated from a fixed pool to avoid heap fragmentation)
    static constexpr int MAX_CLONES = 32;
    ScratchSprite  clonePool[MAX_CLONES];
    bool           cloneUsed[MAX_CLONES];

    void startHatBlocks(BlockOpcode hat, ScratchSprite* sprite,
                        const std::string& field = "");
    int  startHatBlocksCount(BlockOpcode hat, ScratchSprite* sprite,
                              const std::string& field = "");

    void         executeThread(ScriptThread& thread, double dt);
    ScratchValue executeBlock(ScriptThread& thread,
                               const std::string& blockId,
                               bool& yielded);
    ScratchValue evaluateInput(ScriptThread& thread, const ScratchInput& input);
    ScratchValue evaluateReporter(ScriptThread& thread, const std::string& blockId);

    // Block-group handlers (split for readability, inlined by compiler at -O2)
    ScratchValue execMotion(ScriptThread& t,   ScratchBlock* b, bool& yielded);
    ScratchValue execLooks(ScriptThread& t,    ScratchBlock* b, bool& yielded);
    ScratchValue execSound(ScriptThread& t,    ScratchBlock* b, bool& yielded);
    ScratchValue execControl(ScriptThread& t,  ScratchBlock* b, bool& yielded);
    ScratchValue execSensing(ScriptThread& t,  ScratchBlock* b, bool& yielded);
    ScratchValue execData(ScriptThread& t,     ScratchBlock* b, bool& yielded);
    ScratchValue execOperator(ScriptThread& t, ScratchBlock* b, bool& yielded);
    ScratchValue execNDS(ScriptThread& t,      ScratchBlock* b, bool& yielded);

    ScratchBlock* getBlock(ScriptThread& t, const std::string& id);

    // Helper: normalise Scratch direction into (-180, 180]
    static double normDir(double d);

    // Helper: check if two sprites overlap (AABB)
    bool spritesTouching(ScratchSprite* a, ScratchSprite* b) const;
    bool spriteTouchingEdge(ScratchSprite* s) const;

    // Helper: get a named property of a sprite ("x position", "y position", etc.)
    ScratchValue getSpriteProperty(ScratchSprite* s, const std::string& prop);

    static constexpr int MAX_STEPS_PER_FRAME = 1024;
};
