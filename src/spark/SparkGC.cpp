//
// Created by kiva on 2018-12-31.
//
#include <spark/SparkGC.h>
#include <spark/SparkMutator.h>
#include <spark/CollectedObject.h>

namespace spark {

    SparkGC::SparkGC(CollectedHeap *heap)
        : heap(heap), handshakeState(GC_ASYNC), gcStage(GC_RESTING),
          clearColor(GC_COLOR_WHITE), markColor(GC_COLOR_BLACK),
          colorMarker(this) {
        Size max = heap->getMaxBlockCount() + 1;
        for (int i = 0; i < max; ++i) {
            heapColors.createBitmap();
        }
    }

    SparkGC::~SparkGC() = default;

    void SparkGC::collect() {
        stageClear();
        stageMark();
        stageTrace();
        stageSweep();
        stageDone();
    }

    void SparkGC::markGlobalRoot() {
        for (auto block : heap->fullBlocks) {
            markBlock(block);
        }
    }


    void SparkGC::markBlock(HeapBlock *block) {
        // Scan the whole block
        Addr current = block->getStart();
        while (current < block->getEnd()) {
            auto object = (CollectedObject *) current;
            collectorMarkBlack(current);
            emptyMarkBuffer();
            // move 4 bytes forward as the unit of our ColorBitmap is 4 bytes
            current += object->getOnStackSize();
        }
    }

    void SparkGC::processWeakRefs() {
        // Nothing to do
    }

    void SparkGC::collectorTrace() {
        bool clean = false;
        while (!clean) {
            clean = true;
            for (auto m : mutators) {
                while (m->lastRead < m->lastWrite) {
                    clean = false;
                    ++m->lastRead;
                    collectorMarkBlack(m->markBuffer[m->lastRead]);
                    emptyMarkBuffer();
                }
            }
        }
    }

    void SparkGC::collectorSweep() {
        // Scan the whole heap
        Addr current = heap->getHeapStart();
        while (current < heap->getHeapEnd()) {
            Size offset = offsetOf(current);
            GCColor color = heapColors.getColor(offset);
            if (color == clearColor) {
                heap->free(current);
                heapColors.setColor(offset, GC_COLOR_BLUE);
            }
            // move 4 bytes forward as the unit of our ColorBitmap is 4 bytes
            current += 4;
        }
    }

    void SparkGC::stageClear() {
        postState(GC_CLEAR_OR_MARKING);
        while (!weakRefs.empty()) {
            weakRefs.pop();
        }
        while (!markBuffer.empty()) {
            markBuffer.pop();
        }
        handshakeMutators(GC_SYNC1);
    }

    void SparkGC::stageMark() {
        handshakeMutators(GC_SYNC2);
        postState(GC_TRACING);
        postHandshake(GC_ASYNC);
        markGlobalRoot();
        waitHandshake();
    }

    void SparkGC::stageTrace() {
        collectorTrace();
        postState(GC_REF_PROCESSING);
        processWeakRefs();
    }

    void SparkGC::stageSweep() {
        postState(GC_SWEEPING);
        collectorSweep();
    }

    void SparkGC::stageDone() {
        // Change the interpretation of the colors
        // for the next gc cycle.
        toggleColor();
        postState(GC_RESTING);
    }

    /**
     * According to the basic mark-sweep algorithm, the color of
     * all black (marked) objects must be reset to white (clear)
     * in preparation for the next GC cycle. We introduce a color toggle
     * instead of changing the color for each black object.
     * The color toggle simply changes the interpretation of the colors.
     * There are three main advantages to this modification:
     *
     * 1) it reduces the amount of work that the collector must do,
     *    since the collector no longer is required to reset black
     *    items to white in sweep.
     * 2) it simplifies and speeds up object creation by removing
     *    the checks for the location of the sweep pointer.
     * 3) it removes the the store-load dependency in object creation.
     */
    void SparkGC::toggleColor() {
        std::swap(clearColor, markColor);
    }

    void SparkGC::handshakeMutators(GCHandshakeState state) {
        postHandshake(state);
        waitHandshake();
    }

    void SparkGC::waitHandshake() const {
        Size mutatorCount = mutators.size();
        Size readyCount = 0;
        do {
            for (auto m : mutators) {
                if (m->handshakeState == handshakeState) {
                    ++readyCount;
                }
            }
        } while (readyCount != mutatorCount);
    }

    void SparkGC::postHandshake(GCHandshakeState state) {
        handshakeState = state;
    }

    void SparkGC::postState(GCStage stage) {
        gcStage = stage;
    }

    void SparkGC::registerMutator(SparkMutator *mutator) {
        if (mutator == nullptr) {
            throw std::exception();
        }
        mutator->allocationColor = getMarkColor();
        mutator->sparkGC = this;
        this->mutators.insert(mutator);
    }

    void SparkGC::unregisterMutator(SparkMutator *mutator) {
        if (mutator == nullptr) {
            throw std::exception();
        }
        this->mutators.erase(mutator);
    }

    void SparkGC::markGray(SparkMutator *m, Addr addr) {
        Size offset = offsetOf(addr);
        if (heapColors.getColor(offset) == clearColor) {
            if (m->lastWrite >= m->markBuffer.size()) {
                m->markBuffer.push_back(addr);
                m->lastWrite = m->markBuffer.size();
            } else {
                m->markBuffer[m->lastWrite] = addr;
                ++m->lastWrite;
            }
        }
    }

    Size SparkGC::offsetOf(Addr addr) const {
        if (addr < heap->getHeapStart()) {
            throw std::exception();
        }

        return addr - heap->getHeapStart();
    }

    Addr SparkGC::allocate(Size size) {
        return nullptr;
    }

    void SparkGC::setColor(Addr addr, GCColor color) {
        heapColors.setColor(offsetOf(addr), color);
    }

    void SparkGC::collectorMarkGray(Addr addr) {
        if (heapColors.getColor(offsetOf(addr)) == clearColor) {
            markBuffer.push(addr);
        }
    }

    void SparkGC::collectorMarkBlack(Addr addr) {
        if (heapColors.getColor(offsetOf(addr)) != markColor) {
            if (queryIsWeakReference(addr)) {
                weakRefs.push(addr);
                // for each pointer p except its referent
                // collectorMarkGray(p)
                // TODO: support weak reference
            } else {
                // for each pointer p belongs to addr
                // collectorMarkGray(p)
                auto object = (CollectedObject *) addr;
                object->markChildren(colorMarker);
            }
            setColor(addr, markColor);
        }
    }

    bool SparkGC::queryIsWeakReference(Addr addr) {
        return false;
    }

    void SparkGC::emptyMarkBuffer() {
        while (!markBuffer.empty()) {
            Addr addr = markBuffer.top();
            markBuffer.pop();
            collectorMarkBlack(addr);
        }
    }
}
