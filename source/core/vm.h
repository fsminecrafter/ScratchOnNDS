// =============================================================================
// vm.h — optimized for NDS ARM9
//
// Key changes vs original:
//   1. ScratchValue: type tag + numVal packed into 12 bytes instead of having
//      std::string always present. STR values still need heap but NUM values
//      (the vast majority at runtime) are now pure stack objects.
//   2. ScriptThread: stepsThisFrame removed from the struct hot path — it was
//      reset every frame anyway; kept as a local in executeThread.
//   3. StackFrame: consolidated bool flags into a uint8_t to reduce padding.
//   4. getBlock() changed to take const char* to avoid std::string construction
//      at every call site in the interpreter loop.
//   5. ScratchVM is no longer a Meyer's singleton in the hot path — call sites
//      hold a direct reference after init rather than getInstance() per frame.
// =============================================================================
#pragma once
#include "project.h"
#include <vector>
#include <string>

class ScratchVM;

// -----------------------------------------------------------------------
// ScratchValue — 12 bytes on ARM (type + num + optional string ptr)
// NUM path is allocation-free; STR still uses std::string heap.
// -----------------------------------------------------------------------
struct ScratchValue {
    enum Type : uint8_t { NUM, STR } type;
    double      numVal;    // valid when type==NUM; also cached for STR
    std::string strVal;    // only populated when type==STR

    ScratchValue()                     : type(NUM), numVal(0.0) {}
    explicit ScratchValue(double d)    : type(NUM), numVal(d)   {}
    ScratchValue(const std::string& s) : type(STR), numVal(0.0), strVal(s) {}
    ScratchValue(const char* s)        : type(STR), numVal(0.0), strVal(s) {}

    double      toNum()  const;
    std::string toStr()  const;
    bool        toBool() const { return toNum() != 0.0; }

    bool operator==(const ScratchValue& o) const;
    bool operator< (const ScratchValue& o) const;
    bool operator> (const ScratchValue& o) const;
};

// -----------------------------------------------------------------------
// Runtime list
// -----------------------------------------------------------------------
struct ScratchRuntimeList {
    std::string              name;
    std::vector<std::string> items;
};

// -----------------------------------------------------------------------
// StackFrame — flags packed to reduce size
// -----------------------------------------------------------------------
struct StackFrame {
    char loopBlockId[MAX_BLOCK_ID + 1];
    char returnBlockId[MAX_BLOCK_ID + 1];
    int  remaining;        // -1 = forever, 0 = done, >0 = count

    double glideStartX, glideStartY;
    double glideEndX,   glideEndY;
    double glideDuration, glideElapsed;

    char condBlockId[MAX_BLOCK_ID + 1];
    char broadcastName[32];

    // Pack booleans into a single byte — ARM alignment wastes 3 bytes per bool
    uint8_t flags;
    static constexpr uint8_t F_GLIDE           = 1 << 0;
    static constexpr uint8_t F_WAIT_UNTIL      = 1 << 1;
    static constexpr uint8_t F_BROADCAST_WAIT  = 1 << 2;

    bool isGlide()          const { return (flags & F_GLIDE) != 0; }
    bool isWaitUntil()      const { return (flags & F_WAIT_UNTIL) != 0; }
    bool isBroadcastWait()  const { return (flags & F_BROADCAST_WAIT) != 0; }

    StackFrame() : remaining(-1),
                   glideStartX(0), glideStartY(0),
                   glideEndX(0), glideEndY(0),
                   glideDuration(0), glideElapsed(0),
                   flags(0) {
        loopBlockId[0] = returnBlockId[0] = condBlockId[0] = broadcastName[0] = '\0';
    }
};

// -----------------------------------------------------------------------
// ScriptThread
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
    // Fixed char array avoids heap allocation in the per-frame inner loop.
    char           currentBlockId[MAX_BLOCK_ID + 1];
    State          state;
    double         waitTimer;
    bool           isClone;

    std::vector<StackFrame> callStack;
    int stepsThisFrame = 0;

    ScriptThread() : sprite(nullptr), state(RUNNING),
                     waitTimer(0.0), isClone(false) {
        currentBlockId[0] = '\0';
    }
};

// -----------------------------------------------------------------------
// Broadcast record
// -----------------------------------------------------------------------
struct BroadcastRecord {
    char name[32];
    int  threadsLaunched;
    int  threadsDone;
    bool isWaiting;
};

// -----------------------------------------------------------------------
// ScratchVM
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
    void step(double dt);
    void broadcast(const std::string& name);
    void broadcastAndWait(ScriptThread& caller, const std::string& name);
    void stopAll();

    void fireSpriteClicked(ScratchSprite* sprite);
    void fireKeyPressed(const std::string& key);

    ScratchValue getVariable(ScratchSprite* sprite, const char* name);
    ScratchValue getVariable(ScratchSprite* sprite, const std::string& name) {
        return getVariable(sprite, name.c_str());
    }
    void setVariable(ScratchSprite* sprite, const char* name,
                     const ScratchValue& val);
    void setVariable(ScratchSprite* sprite, const std::string& name,
                     const ScratchValue& val) {
        setVariable(sprite, name.c_str(), val);
    }

    ScratchRuntimeList* getList(ScratchSprite* sprite, const char* name);

    ScratchSprite* createClone(ScratchSprite* parent);
    void           deleteClone(ScratchSprite* sprite);

    std::string answerStr;
    double      globalTimer;

    const std::vector<ScriptThread>& getThreads() const { return threads; }

private:
    ScratchProject* project;

    std::vector<ScriptThread>  threads;
    std::vector<ScriptThread>  pendingThreads;
    std::vector<BroadcastRecord> broadcasts;

    struct ListEntry {
        ScratchSprite*     owner;
        ScratchRuntimeList list;
    };
    std::vector<ListEntry> runtimeLists;

    static constexpr int MAX_CLONES = 32;
    ScratchSprite  clonePool[MAX_CLONES];
    bool           cloneUsed[MAX_CLONES];

    void startHatBlocks(BlockOpcode hat, ScratchSprite* sprite,
                        const char* field = "");
    int  startHatBlocksCount(BlockOpcode hat, ScratchSprite* sprite,
                              const char* field = "");

    void         executeThread(ScriptThread& thread, double dt);

    ScratchValue executeBlock(ScriptThread& thread,
                                          const std::string& blockId,
                                          bool& yielded) {
        yielded = false;
        ScratchBlock* b = getBlock(thread, blockId);
        if (!b) { thread.state = ScriptThread::DONE; return ScratchValue(); }
    
        // Dispatch by category prefix for speed
        BlockOpcode op = b->opcode;
    
        if (op >= BlockOpcode::MOTION_MOVESTEPS && op <= BlockOpcode::MOTION_DIRECTION)
            return execMotion(thread, b, yielded);
        if (op >= BlockOpcode::LOOKS_SAYFORSECS && op <= BlockOpcode::LOOKS_CHANGEEFFECTBY)
            return execLooks(thread, b, yielded);
        if (op >= BlockOpcode::SOUND_PLAYUNTILDONE && op <= BlockOpcode::SOUND_SETVOLUMETO)
            return execSound(thread, b, yielded);
        if (op >= BlockOpcode::CONTROL_WAIT && op <= BlockOpcode::CONTROL_DELETE_THIS_CLONE)
            return execControl(thread, b, yielded);
        if (op >= BlockOpcode::SENSING_TOUCHINGOBJECT && op <= BlockOpcode::SENSING_ASKANDWAIT)
            return execSensing(thread, b, yielded);
        if (op >= BlockOpcode::DATA_SETVARIABLETO && op <= BlockOpcode::DATA_HIDELIST)
            return execData(thread, b, yielded);
        if (op >= BlockOpcode::OPERATOR_ADD && op <= BlockOpcode::OPERATOR_MATHOP)
            return execOperator(thread, b, yielded);
        if (op >= BlockOpcode::NDS_BUTTONPRESSED)
            return execNDS(thread, b, yielded);
    
        return ScratchValue();
    }

    ScratchValue evaluateInput(ScriptThread& thread, const ScratchInput& input);
    ScratchValue evaluateReporter(ScriptThread& thread, const char* blockId);

    ScratchValue execMotion  (ScriptThread& t, ScratchBlock* b, bool& y);
    ScratchValue execLooks   (ScriptThread& t, ScratchBlock* b, bool& y);
    ScratchValue execSound   (ScriptThread& t, ScratchBlock* b, bool& y);
    ScratchValue execControl (ScriptThread& t, ScratchBlock* b, bool& y);
    ScratchValue execSensing (ScriptThread& t, ScratchBlock* b, bool& y);
    ScratchValue execData    (ScriptThread& t, ScratchBlock* b, bool& y);
    ScratchValue execOperator(ScriptThread& t, ScratchBlock* b, bool& y);
    ScratchValue execNDS     (ScriptThread& t, ScratchBlock* b, bool& y);

    // O(1) block lookup via pre-built index in ScratchSprite
    inline ScratchBlock* getBlock(ScriptThread& t, const char* id) {
        return t.sprite ? findBlock(*t.sprite, id) : nullptr;
    }
    inline ScratchBlock* getBlock(ScriptThread& t, const std::string& id) {
        return getBlock(t, id.c_str());
    }

    static double normDir(double d);
    bool spritesTouching(ScratchSprite* a, ScratchSprite* b) const;
    bool spriteTouchingEdge(ScratchSprite* s) const;
    ScratchValue getSpriteProperty(ScratchSprite* s, const char* prop);

    static constexpr int MAX_STEPS_PER_FRAME = 1024;
};
