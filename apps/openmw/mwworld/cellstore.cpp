#include "cellstore.hpp"

#include <algorithm>

#include <components/debug/debuglog.hpp>

#include <components/esm/cellstate.hpp>
#include <components/esm/cellid.hpp>
#include <components/esm/cellref.hpp>
#include <components/esm/esmreader.hpp>
#include <components/esm/esmwriter.hpp>
#include <components/esm/objectstate.hpp>
#include <components/esm/containerstate.hpp>
#include <components/esm/npcstate.hpp>
#include <components/esm/creaturestate.hpp>
#include <components/esm/fogstate.hpp>
#include <components/esm/creaturelevliststate.hpp>
#include <components/esm/doorstate.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/luamanager.hpp"
#include "../mwbase/mechanicsmanager.hpp"
#include "../mwbase/world.hpp"

#include "../mwmechanics/creaturestats.hpp"
#include "../mwmechanics/recharge.hpp"

#include "ptr.hpp"
#include "esmloader.hpp"
#include "esmstore.hpp"
#include "class.hpp"
#include "containerstore.hpp"

namespace
{
    template<typename T>
    MWWorld::Ptr searchInContainerList (MWWorld::CellRefList<T>& containerList, const std::string& id)
    {
        for (typename MWWorld::CellRefList<T>::List::iterator iter (containerList.mList.begin());
             iter!=containerList.mList.end(); ++iter)
        {
            MWWorld::Ptr container (&*iter, nullptr);

            if (container.getRefData().getCustomData() == nullptr)
                continue;

            MWWorld::Ptr ptr =
                container.getClass().getContainerStore (container).search (id);

            if (!ptr.isEmpty())
                return ptr;
        }

        return MWWorld::Ptr();
    }

    template<typename T>
    MWWorld::Ptr searchViaActorId (MWWorld::CellRefList<T>& actorList, int actorId,
        MWWorld::CellStore *cell, const std::map<MWWorld::LiveCellRefBase*, MWWorld::CellStore*>& toIgnore)
    {
        for (typename MWWorld::CellRefList<T>::List::iterator iter (actorList.mList.begin());
             iter!=actorList.mList.end(); ++iter)
        {
            MWWorld::Ptr actor (&*iter, cell);

            if (toIgnore.find(&*iter) != toIgnore.end())
                continue;

            if (actor.getClass().getCreatureStats (actor).matchesActorId (actorId) && actor.getRefData().getCount() > 0)
                return actor;
        }

        return MWWorld::Ptr();
    }

    template<typename RecordType, typename T>
    void writeReferenceCollection (ESM::ESMWriter& writer,
        const MWWorld::CellRefList<T>& collection)
    {
        if (!collection.mList.empty())
        {
            // references
            for (typename MWWorld::CellRefList<T>::List::const_iterator
                iter (collection.mList.begin());
                iter!=collection.mList.end(); ++iter)
            {
                if (!iter->mData.hasChanged() && !iter->mRef.hasChanged() && iter->mRef.hasContentFile())
                {
                    // Reference that came from a content file and has not been changed -> ignore
                    continue;
                }
                if (iter->mData.getCount()==0 && !iter->mRef.hasContentFile())
                {
                    // Deleted reference that did not come from a content file -> ignore
                    continue;
                }

                RecordType state;
                iter->save (state);

                // recordId currently unused
                writer.writeHNT ("OBJE", collection.mList.front().mBase->sRecordId);

                state.save (writer);
            }
        }
    }

    template<class RecordType, class T>
    void fixRestockingImpl(const T* base, RecordType& state)
    {
        // Workaround for old saves not containing negative quantities
        for(const auto& baseItem : base->mInventory.mList)
        {
            if(baseItem.mCount < 0)
            {
                for(auto& item : state.mInventory.mItems)
                {
                    if(item.mCount > 0 && Misc::StringUtils::ciEqual(baseItem.mItem, item.mRef.mRefID))
                        item.mCount = -item.mCount;
                }
            }
        }
    }

    template<class RecordType, class T>
    void fixRestocking(const T* base, RecordType& state)
    {}

    template<>
    void fixRestocking<>(const ESM::Creature* base, ESM::CreatureState& state)
    {
        fixRestockingImpl(base, state);
    }

    template<>
    void fixRestocking<>(const ESM::NPC* base, ESM::NpcState& state)
    {
        fixRestockingImpl(base, state);
    }

    template<>
    void fixRestocking<>(const ESM::Container* base, ESM::ContainerState& state)
    {
        fixRestockingImpl(base, state);
    }

    template<typename RecordType, typename T>
    void readReferenceCollection (ESM::ESMReader& reader,
        MWWorld::CellRefList<T>& collection, const ESM::CellRef& cref, const std::map<int, int>& contentFileMap, MWWorld::CellStore* cellstore)
    {
        const MWWorld::ESMStore& esmStore = MWBase::Environment::get().getWorld()->getStore();

        RecordType state;
        state.mRef = cref;
        state.load(reader);

        // If the reference came from a content file, make sure this content file is loaded
        if (state.mRef.mRefNum.hasContentFile())
        {
            std::map<int, int>::const_iterator iter =
                contentFileMap.find (state.mRef.mRefNum.mContentFile);

            if (iter==contentFileMap.end())
                return; // content file has been removed -> skip

            state.mRef.mRefNum.mContentFile = iter->second;
        }

        if (!MWWorld::LiveCellRef<T>::checkState (state))
            return; // not valid anymore with current content files -> skip

        const T *record = esmStore.get<T>().search (state.mRef.mRefID);

        if (!record)
            return;

        if (state.mVersion < 15)
            fixRestocking(record, state);
        if (state.mVersion < 17)
        {
            if constexpr (std::is_same_v<T, ESM::Creature>)
                MWWorld::convertMagicEffects(state.mCreatureStats, state.mInventory);
            else if constexpr (std::is_same_v<T, ESM::NPC>)
                MWWorld::convertMagicEffects(state.mCreatureStats, state.mInventory, &state.mNpcStats);
        }

        if (state.mRef.mRefNum.hasContentFile())
        {
            for (typename MWWorld::CellRefList<T>::List::iterator iter (collection.mList.begin());
                iter!=collection.mList.end(); ++iter)
                if (iter->mRef.getRefNum()==state.mRef.mRefNum && iter->mRef.getRefIdRef() == state.mRef.mRefID)
                {
                    // overwrite existing reference
                    float oldscale = iter->mRef.getScale();
                    iter->load (state);
                    const ESM::Position & oldpos = iter->mRef.getPosition();
                    const ESM::Position & newpos = iter->mData.getPosition();
                    const MWWorld::Ptr ptr(&*iter, cellstore);
                    if ((oldscale != iter->mRef.getScale() || oldpos.asVec3() != newpos.asVec3() || oldpos.rot[0] != newpos.rot[0] || oldpos.rot[1] != newpos.rot[1] || oldpos.rot[2] != newpos.rot[2]) && !ptr.getClass().isActor())
                        MWBase::Environment::get().getWorld()->moveObject(ptr, newpos.asVec3());
                    if (!iter->mData.isEnabled())
                    {
                        iter->mData.enable();
                        MWBase::Environment::get().getWorld()->disable(MWWorld::Ptr(&*iter, cellstore));
                    }
                    else
                        MWBase::Environment::get().getLuaManager()->registerObject(MWWorld::Ptr(&*iter, cellstore));
                    return;
                }

            Log(Debug::Warning) << "Warning: Dropping reference to " << state.mRef.mRefID << " (invalid content file link)";
            return;
        }

        // new reference
        MWWorld::LiveCellRef<T> ref (record);
        ref.load (state);
        collection.mList.push_back (ref);

        MWWorld::LiveCellRefBase* base = &collection.mList.back();
        MWBase::Environment::get().getLuaManager()->registerObject(MWWorld::Ptr(base, cellstore));
    }
}

namespace MWWorld
{

    template <typename X>
    void CellRefList<X>::load(ESM::CellRef &ref, bool deleted, const MWWorld::ESMStore &esmStore)
    {
        const MWWorld::Store<X> &store = esmStore.get<X>();

        if (const X *ptr = store.search (ref.mRefID))
        {
            typename std::list<LiveRef>::iterator iter =
                std::find(mList.begin(), mList.end(), ref.mRefNum);

            LiveRef liveCellRef (ref, ptr);

            if (deleted)
                liveCellRef.mData.setDeletedByContentFile(true);

            if (iter != mList.end())
                *iter = liveCellRef;
            else
                mList.push_back (liveCellRef);
        }
        else
        {
            Log(Debug::Warning)
                << "Warning: could not resolve cell reference '" << ref.mRefID << "'"
                << " (dropping reference)";
        }
    }

    template<typename X> bool operator==(const LiveCellRef<X>& ref, int pRefnum)
    {
        return (ref.mRef.mRefnum == pRefnum);
    }

    Ptr CellStore::getCurrentPtr(LiveCellRefBase *ref)
    {
        MovedRefTracker::iterator found = mMovedToAnotherCell.find(ref);
        if (found != mMovedToAnotherCell.end())
            return Ptr(ref, found->second);
        return Ptr(ref, this);
    }

    void CellStore::moveFrom(const Ptr &object, CellStore *from)
    {
        if (mState != State_Loaded)
            load();

        mHasState = true;
        MovedRefTracker::iterator found = mMovedToAnotherCell.find(object.getBase());
        if (found != mMovedToAnotherCell.end())
        {
            // A cell we had previously moved an object to is returning it to us.
            assert (found->second == from);
            mMovedToAnotherCell.erase(found);
        }
        else
        {
            mMovedHere.insert(std::make_pair(object.getBase(), from));
        }
        updateMergedRefs();
    }

    MWWorld::Ptr CellStore::moveTo(const Ptr &object, CellStore *cellToMoveTo)
    {
        if (cellToMoveTo == this)
            throw std::runtime_error("moveTo: object is already in this cell");

        // We assume that *this is in State_Loaded since we could hardly have reference to a live object otherwise.
        if (mState != State_Loaded)
            throw std::runtime_error("moveTo: can't move object from a non-loaded cell (how did you get this object anyway?)");

        // Ensure that the object actually exists in the cell
        if (searchViaRefNum(object.getCellRef().getRefNum()).isEmpty())
            throw std::runtime_error("moveTo: object is not in this cell");

        MWBase::Environment::get().getLuaManager()->registerObject(MWWorld::Ptr(object.getBase(), cellToMoveTo));

        MovedRefTracker::iterator found = mMovedHere.find(object.getBase());
        if (found != mMovedHere.end())
        {
            // Special case - object didn't originate in this cell
            // Move it back to its original cell first
            CellStore* originalCell = found->second;
            assert (originalCell != this);
            originalCell->moveFrom(object, this);

            mMovedHere.erase(found);

            // Now that object is back to its rightful owner, we can move it
            if (cellToMoveTo != originalCell)
            {
                originalCell->moveTo(object, cellToMoveTo);
            }

            updateMergedRefs();
            return MWWorld::Ptr(object.getBase(), cellToMoveTo);
        }

        cellToMoveTo->moveFrom(object, this);
        mMovedToAnotherCell.insert(std::make_pair(object.getBase(), cellToMoveTo));

        updateMergedRefs();
        return MWWorld::Ptr(object.getBase(), cellToMoveTo);
    }

    struct MergeVisitor
    {
        MergeVisitor(std::vector<LiveCellRefBase*>& mergeTo, const std::map<LiveCellRefBase*, MWWorld::CellStore*>& movedHere,
                     const std::map<LiveCellRefBase*, MWWorld::CellStore*>& movedToAnotherCell)
            : mMergeTo(mergeTo)
            , mMovedHere(movedHere)
            , mMovedToAnotherCell(movedToAnotherCell)
        {
        }

        bool operator() (const MWWorld::Ptr& ptr)
        {
            if (mMovedToAnotherCell.find(ptr.getBase()) != mMovedToAnotherCell.end())
                return true;
            mMergeTo.push_back(ptr.getBase());
            return true;
        }

        void merge()
        {
            for (const auto & [base, _] : mMovedHere)
                mMergeTo.push_back(base);
        }

    private:
        std::vector<LiveCellRefBase*>& mMergeTo;

        const std::map<LiveCellRefBase*, MWWorld::CellStore*>& mMovedHere;
        const std::map<LiveCellRefBase*, MWWorld::CellStore*>& mMovedToAnotherCell;
    };

    void CellStore::updateMergedRefs()
    {
        mMergedRefs.clear();
        mRechargingItemsUpToDate = false;
        MergeVisitor visitor(mMergedRefs, mMovedHere, mMovedToAnotherCell);
        forEachInternal(visitor);
        visitor.merge();
    }

    bool CellStore::movedHere(const MWWorld::Ptr& ptr) const
    {
        if (ptr.isEmpty())
            return false;

        if (mMovedHere.find(ptr.getBase()) != mMovedHere.end())
            return true;

        return false;
    }

    CellStore::CellStore (const ESM::Cell *cell, const MWWorld::ESMStore& esmStore, std::vector<ESM::ESMReader>& readerList)
        : mStore(esmStore), mReader(readerList), mCell (cell), mState (State_Unloaded), mHasState (false), mLastRespawn(0,0), mRechargingItemsUpToDate(false)
    {
        mWaterLevel = cell->mWater;
    }

    const ESM::Cell *CellStore::getCell() const
    {
        return mCell;
    }

    CellStore::State CellStore::getState() const
    {
        return mState;
    }

    const std::vector<std::string> &CellStore::getPreloadedIds() const
    {
        return mIds;
    }

    bool CellStore::hasState() const
    {
        return mHasState;
    }

    bool CellStore::hasId (const std::string& id) const
    {
        if (mState==State_Unloaded)
            return false;

        if (mState==State_Preloaded)
            return std::binary_search (mIds.begin(), mIds.end(), id);

        return searchConst (id).isEmpty();
    }

    template <typename PtrType>
    struct SearchVisitor
    {
        PtrType mFound;
        const std::string *mIdToFind;
        bool operator()(const PtrType& ptr)
        {
            if (ptr.getCellRef().getRefIdRef() == *mIdToFind)
            {
                mFound = ptr;
                return false;
            }
            return true;
        }
    };

    Ptr CellStore::search (const std::string& id)
    {
        SearchVisitor<MWWorld::Ptr> searchVisitor;
        searchVisitor.mIdToFind = &id;
        forEach(searchVisitor);
        return searchVisitor.mFound;
    }

    ConstPtr CellStore::searchConst (const std::string& id) const
    {
        SearchVisitor<MWWorld::ConstPtr> searchVisitor;
        searchVisitor.mIdToFind = &id;
        forEachConst(searchVisitor);
        return searchVisitor.mFound;
    }

    Ptr CellStore::searchViaActorId (int id)
    {
        if (Ptr ptr = ::searchViaActorId (mNpcs, id, this, mMovedToAnotherCell))
            return ptr;

        if (Ptr ptr = ::searchViaActorId (mCreatures, id, this, mMovedToAnotherCell))
            return ptr;

        for (const auto& [base, _] : mMovedHere)
        {
            MWWorld::Ptr actor (base, this);
            if (!actor.getClass().isActor())
                continue;
            if (actor.getClass().getCreatureStats (actor).matchesActorId (id) && actor.getRefData().getCount() > 0)
                return actor;
        }

        return Ptr();
    }

    class RefNumSearchVisitor
    {
        const ESM::RefNum& mRefNum;
    public:
        RefNumSearchVisitor(const ESM::RefNum& refNum) : mRefNum(refNum) {}

        Ptr mFound;

        bool operator()(const Ptr& ptr)
        {
            if (ptr.getCellRef().getRefNum() == mRefNum)
            {
                mFound = ptr;
                return false;
            }
            return true;
        }
    };

    Ptr CellStore::searchViaRefNum (const ESM::RefNum& refNum)
    {
        RefNumSearchVisitor searchVisitor(refNum);
        forEach(searchVisitor);
        return searchVisitor.mFound;
    }

    float CellStore::getWaterLevel() const
    {
        if (isExterior())
            return -1;
        return mWaterLevel;
    }

    void CellStore::setWaterLevel (float level)
    {
        mWaterLevel = level;
        mHasState = true;
    }

    std::size_t CellStore::count() const
    {
        return mMergedRefs.size();
    }

    void CellStore::load ()
    {
        if (mState!=State_Loaded)
        {
            if (mState==State_Preloaded)
                mIds.clear();

            loadRefs ();

            mState = State_Loaded;
        }
    }

    void CellStore::preload ()
    {
        if (mState==State_Unloaded)
        {
            listRefs ();

            mState = State_Preloaded;
        }
    }

    void CellStore::listRefs()
    {
        std::vector<ESM::ESMReader>& esm = mReader;

        assert (mCell);

        if (mCell->mContextList.empty())
            return; // this is a dynamically generated cell -> skipping.

        // Load references from all plugins that do something with this cell.
        for (size_t i = 0; i < mCell->mContextList.size(); i++)
        {
            try
            {
                // Reopen the ESM reader and seek to the right position.
                int index = mCell->mContextList[i].index;
                mCell->restore (esm[index], i);

                ESM::CellRef ref;

                // Get each reference in turn
                ESM::MovedCellRef cMRef;
                cMRef.mRefNum.mIndex = 0;
                bool deleted = false;
                bool moved = false;
                while(mCell->getNextRef(esm[index], ref, deleted, cMRef, moved))
                {
                    if (deleted || moved)
                        continue;

                    // Don't list reference if it was moved to a different cell.
                    ESM::MovedCellRefTracker::const_iterator iter =
                        std::find(mCell->mMovedRefs.begin(), mCell->mMovedRefs.end(), ref.mRefNum);
                    if (iter != mCell->mMovedRefs.end()) {
                        continue;
                    }

                    Misc::StringUtils::lowerCaseInPlace(ref.mRefID);
                    mIds.push_back(std::move(ref.mRefID));
                }
            }
            catch (std::exception& e)
            {
                Log(Debug::Error) << "An error occurred listing references for cell " << getCell()->getDescription() << ": " << e.what();
            }
        }

        // List moved references, from separately tracked list.
        for (const auto& [ref, deleted]: mCell->mLeasedRefs)
        {
            if (!deleted)
                mIds.push_back(Misc::StringUtils::lowerCase(ref.mRefID));
        }

        std::sort (mIds.begin(), mIds.end());
    }

    void CellStore::loadRefs()
    {
        std::vector<ESM::ESMReader>& esm = mReader;

        assert (mCell);

        if (mCell->mContextList.empty())
            return; // this is a dynamically generated cell -> skipping.

        std::map<ESM::RefNum, std::string> refNumToID; // used to detect refID modifications

        // Load references from all plugins that do something with this cell.
        for (size_t i = 0; i < mCell->mContextList.size(); i++)
        {
            try
            {
                // Reopen the ESM reader and seek to the right position.
                int index = mCell->mContextList[i].index;
                mCell->restore (esm[index], i);

                ESM::CellRef ref;
                ref.mRefNum.unset();

                // Get each reference in turn
                ESM::MovedCellRef cMRef;
                cMRef.mRefNum.mIndex = 0;
                bool deleted = false;
                bool moved = false;
                while(mCell->getNextRef(esm[index], ref, deleted, cMRef, moved))
                {
                    if (moved)
                        continue;

                    // Don't load reference if it was moved to a different cell.
                    ESM::MovedCellRefTracker::const_iterator iter =
                        std::find(mCell->mMovedRefs.begin(), mCell->mMovedRefs.end(), ref.mRefNum);
                    if (iter != mCell->mMovedRefs.end()) {
                        continue;
                    }

                    loadRef (ref, deleted, refNumToID);
                }
            }
            catch (std::exception& e)
            {
                Log(Debug::Error) << "An error occurred loading references for cell " << getCell()->getDescription() << ": " << e.what();
            }
        }

        // Load moved references, from separately tracked list.
        for (const auto& leasedRef : mCell->mLeasedRefs)
        {
            ESM::CellRef &ref = const_cast<ESM::CellRef&>(leasedRef.first);
            bool deleted = leasedRef.second;

            loadRef (ref, deleted, refNumToID);
        }

        updateMergedRefs();
    }

    bool CellStore::isExterior() const
    {
        return mCell->isExterior();
    }

    Ptr CellStore::searchInContainer (const std::string& id)
    {
        bool oldState = mHasState;

        mHasState = true;

        if (Ptr ptr = searchInContainerList (mContainers, id))
            return ptr;

        if (Ptr ptr = searchInContainerList (mCreatures, id))
            return ptr;

        if (Ptr ptr = searchInContainerList (mNpcs, id))
            return ptr;

        mHasState = oldState;

        return Ptr();
    }

    void CellStore::loadRef (ESM::CellRef& ref, bool deleted, std::map<ESM::RefNum, std::string>& refNumToID)
    {
        Misc::StringUtils::lowerCaseInPlace (ref.mRefID);

        const MWWorld::ESMStore& store = mStore;

        std::map<ESM::RefNum, std::string>::iterator it = refNumToID.find(ref.mRefNum);
        if (it != refNumToID.end())
        {
            if (it->second != ref.mRefID)
            {
                // refID was modified, make sure we don't end up with duplicated refs
                switch (store.find(it->second))
                {
                    case ESM::REC_ACTI: mActivators.remove(ref.mRefNum); break;
                    case ESM::REC_ALCH: mPotions.remove(ref.mRefNum); break;
                    case ESM::REC_APPA: mAppas.remove(ref.mRefNum); break;
                    case ESM::REC_ARMO: mArmors.remove(ref.mRefNum); break;
                    case ESM::REC_BOOK: mBooks.remove(ref.mRefNum); break;
                    case ESM::REC_CLOT: mClothes.remove(ref.mRefNum); break;
                    case ESM::REC_CONT: mContainers.remove(ref.mRefNum); break;
                    case ESM::REC_CREA: mCreatures.remove(ref.mRefNum); break;
                    case ESM::REC_DOOR: mDoors.remove(ref.mRefNum); break;
                    case ESM::REC_INGR: mIngreds.remove(ref.mRefNum); break;
                    case ESM::REC_LEVC: mCreatureLists.remove(ref.mRefNum); break;
                    case ESM::REC_LEVI: mItemLists.remove(ref.mRefNum); break;
                    case ESM::REC_LIGH: mLights.remove(ref.mRefNum); break;
                    case ESM::REC_LOCK: mLockpicks.remove(ref.mRefNum); break;
                    case ESM::REC_MISC: mMiscItems.remove(ref.mRefNum); break;
                    case ESM::REC_NPC_: mNpcs.remove(ref.mRefNum); break;
                    case ESM::REC_PROB: mProbes.remove(ref.mRefNum); break;
                    case ESM::REC_REPA: mRepairs.remove(ref.mRefNum); break;
                    case ESM::REC_STAT: mStatics.remove(ref.mRefNum); break;
                    case ESM::REC_WEAP: mWeapons.remove(ref.mRefNum); break;
                    case ESM::REC_BODY: mBodyParts.remove(ref.mRefNum); break;
                    default:
                        break;
                }
            }
        }

        switch (store.find (ref.mRefID))
        {
            case ESM::REC_ACTI: mActivators.load(ref, deleted, store); break;
            case ESM::REC_ALCH: mPotions.load(ref, deleted,store); break;
            case ESM::REC_APPA: mAppas.load(ref, deleted, store); break;
            case ESM::REC_ARMO: mArmors.load(ref, deleted, store); break;
            case ESM::REC_BOOK: mBooks.load(ref, deleted, store); break;
            case ESM::REC_CLOT: mClothes.load(ref, deleted, store); break;
            case ESM::REC_CONT: mContainers.load(ref, deleted, store); break;
            case ESM::REC_CREA: mCreatures.load(ref, deleted, store); break;
            case ESM::REC_DOOR: mDoors.load(ref, deleted, store); break;
            case ESM::REC_INGR: mIngreds.load(ref, deleted, store); break;
            case ESM::REC_LEVC: mCreatureLists.load(ref, deleted, store); break;
            case ESM::REC_LEVI: mItemLists.load(ref, deleted, store); break;
            case ESM::REC_LIGH: mLights.load(ref, deleted, store); break;
            case ESM::REC_LOCK: mLockpicks.load(ref, deleted, store); break;
            case ESM::REC_MISC: mMiscItems.load(ref, deleted, store); break;
            case ESM::REC_NPC_: mNpcs.load(ref, deleted, store); break;
            case ESM::REC_PROB: mProbes.load(ref, deleted, store); break;
            case ESM::REC_REPA: mRepairs.load(ref, deleted, store); break;
            case ESM::REC_STAT: mStatics.load(ref, deleted, store); break;
            case ESM::REC_WEAP: mWeapons.load(ref, deleted, store); break;
            case ESM::REC_BODY: mBodyParts.load(ref, deleted, store); break;

            case 0: Log(Debug::Error) << "Cell reference '" + ref.mRefID + "' not found!"; return;

            default:
                Log(Debug::Error) << "Error: Ignoring reference '" << ref.mRefID << "' of unhandled type";
                return;
        }

        refNumToID[ref.mRefNum] = ref.mRefID;
    }

    void CellStore::loadState (const ESM::CellState& state)
    {
        mHasState = true;

        if (mCell->mData.mFlags & ESM::Cell::Interior && mCell->mData.mFlags & ESM::Cell::HasWater)
            mWaterLevel = state.mWaterLevel;

        mLastRespawn = MWWorld::TimeStamp(state.mLastRespawn);
    }

    void CellStore::saveState (ESM::CellState& state) const
    {
        state.mId = mCell->getCellId();

        if (mCell->mData.mFlags & ESM::Cell::Interior && mCell->mData.mFlags & ESM::Cell::HasWater)
            state.mWaterLevel = mWaterLevel;

        state.mHasFogOfWar = (mFogState.get() ? 1 : 0);
        state.mLastRespawn = mLastRespawn.toEsm();
    }

    void CellStore::writeFog(ESM::ESMWriter &writer) const
    {
        if (mFogState.get())
        {
            mFogState->save(writer, mCell->mData.mFlags & ESM::Cell::Interior);
        }
    }

    void CellStore::readFog(ESM::ESMReader &reader)
    {
        mFogState.reset(new ESM::FogState());
        mFogState->load(reader);
    }

    void CellStore::writeReferences (ESM::ESMWriter& writer) const
    {
        writeReferenceCollection<ESM::ObjectState> (writer, mActivators);
        writeReferenceCollection<ESM::ObjectState> (writer, mPotions);
        writeReferenceCollection<ESM::ObjectState> (writer, mAppas);
        writeReferenceCollection<ESM::ObjectState> (writer, mArmors);
        writeReferenceCollection<ESM::ObjectState> (writer, mBooks);
        writeReferenceCollection<ESM::ObjectState> (writer, mClothes);
        writeReferenceCollection<ESM::ContainerState> (writer, mContainers);
        writeReferenceCollection<ESM::CreatureState> (writer, mCreatures);
        writeReferenceCollection<ESM::DoorState> (writer, mDoors);
        writeReferenceCollection<ESM::ObjectState> (writer, mIngreds);
        writeReferenceCollection<ESM::CreatureLevListState> (writer, mCreatureLists);
        writeReferenceCollection<ESM::ObjectState> (writer, mItemLists);
        writeReferenceCollection<ESM::ObjectState> (writer, mLights);
        writeReferenceCollection<ESM::ObjectState> (writer, mLockpicks);
        writeReferenceCollection<ESM::ObjectState> (writer, mMiscItems);
        writeReferenceCollection<ESM::NpcState> (writer, mNpcs);
        writeReferenceCollection<ESM::ObjectState> (writer, mProbes);
        writeReferenceCollection<ESM::ObjectState> (writer, mRepairs);
        writeReferenceCollection<ESM::ObjectState> (writer, mStatics);
        writeReferenceCollection<ESM::ObjectState> (writer, mWeapons);
        writeReferenceCollection<ESM::ObjectState> (writer, mBodyParts);

        for (const auto& [base, store] : mMovedToAnotherCell)
        {
            ESM::RefNum refNum = base->mRef.getRefNum();
            ESM::CellId movedTo = store->getCell()->getCellId();

            refNum.save(writer, true, "MVRF");
            movedTo.save(writer);
        }
    }

    void CellStore::readReferences (ESM::ESMReader& reader, const std::map<int, int>& contentFileMap, GetCellStoreCallback* callback)
    {
        mHasState = true;

        while (reader.isNextSub ("OBJE"))
        {
            unsigned int unused;
            reader.getHT (unused);

            // load the RefID first so we know what type of object it is
            ESM::CellRef cref;
            cref.loadId(reader, true);

            int type = MWBase::Environment::get().getWorld()->getStore().find(cref.mRefID);
            if (type == 0)
            {
                Log(Debug::Warning) << "Dropping reference to '" << cref.mRefID << "' (object no longer exists)";
                // Skip until the next OBJE or MVRF
                while(reader.hasMoreSubs() && !reader.peekNextSub("OBJE") && !reader.peekNextSub("MVRF"))
                {
                    reader.getSubName();
                    reader.skipHSub();
                }
                continue;
            }

            switch (type)
            {
                case ESM::REC_ACTI:

                    readReferenceCollection<ESM::ObjectState> (reader, mActivators, cref, contentFileMap, this);
                    break;

                case ESM::REC_ALCH:

                    readReferenceCollection<ESM::ObjectState> (reader, mPotions, cref, contentFileMap, this);
                    break;

                case ESM::REC_APPA:

                    readReferenceCollection<ESM::ObjectState> (reader, mAppas, cref, contentFileMap, this);
                    break;

                case ESM::REC_ARMO:

                    readReferenceCollection<ESM::ObjectState> (reader, mArmors, cref, contentFileMap, this);
                    break;

                case ESM::REC_BOOK:

                    readReferenceCollection<ESM::ObjectState> (reader, mBooks, cref, contentFileMap, this);
                    break;

                case ESM::REC_CLOT:

                    readReferenceCollection<ESM::ObjectState> (reader, mClothes, cref, contentFileMap, this);
                    break;

                case ESM::REC_CONT:

                    readReferenceCollection<ESM::ContainerState> (reader, mContainers, cref, contentFileMap, this);
                    break;

                case ESM::REC_CREA:

                    readReferenceCollection<ESM::CreatureState> (reader, mCreatures, cref, contentFileMap, this);
                    break;

                case ESM::REC_DOOR:

                    readReferenceCollection<ESM::DoorState> (reader, mDoors, cref, contentFileMap, this);
                    break;

                case ESM::REC_INGR:

                    readReferenceCollection<ESM::ObjectState> (reader, mIngreds, cref, contentFileMap, this);
                    break;

                case ESM::REC_LEVC:

                    readReferenceCollection<ESM::CreatureLevListState> (reader, mCreatureLists, cref, contentFileMap, this);
                    break;

                case ESM::REC_LEVI:

                    readReferenceCollection<ESM::ObjectState> (reader, mItemLists, cref, contentFileMap, this);
                    break;

                case ESM::REC_LIGH:

                    readReferenceCollection<ESM::ObjectState> (reader, mLights, cref, contentFileMap, this);
                    break;

                case ESM::REC_LOCK:

                    readReferenceCollection<ESM::ObjectState> (reader, mLockpicks, cref, contentFileMap, this);
                    break;

                case ESM::REC_MISC:

                    readReferenceCollection<ESM::ObjectState> (reader, mMiscItems, cref, contentFileMap, this);
                    break;

                case ESM::REC_NPC_:

                    readReferenceCollection<ESM::NpcState> (reader, mNpcs, cref, contentFileMap, this);
                    break;

                case ESM::REC_PROB:

                    readReferenceCollection<ESM::ObjectState> (reader, mProbes, cref, contentFileMap, this);
                    break;

                case ESM::REC_REPA:

                    readReferenceCollection<ESM::ObjectState> (reader, mRepairs, cref, contentFileMap, this);
                    break;

                case ESM::REC_STAT:

                    readReferenceCollection<ESM::ObjectState> (reader, mStatics, cref, contentFileMap, this);
                    break;

                case ESM::REC_WEAP:

                    readReferenceCollection<ESM::ObjectState> (reader, mWeapons, cref, contentFileMap, this);
                    break;

                case ESM::REC_BODY:

                    readReferenceCollection<ESM::ObjectState> (reader, mBodyParts, cref, contentFileMap, this);
                    break;

                default:

                    throw std::runtime_error ("unknown type in cell reference section");
            }
        }

        // Do another update here to make sure objects referred to by MVRF tags can be found
        // This update is only needed for old saves that used the old copy&delete way of moving objects
        updateMergedRefs();

        while (reader.isNextSub("MVRF"))
        {
            reader.cacheSubName();
            ESM::RefNum refnum;
            ESM::CellId movedTo;
            refnum.load(reader, true, "MVRF");
            movedTo.load(reader);

            if (refnum.hasContentFile())
            {
                auto iter = contentFileMap.find(refnum.mContentFile);
                if (iter != contentFileMap.end())
                    refnum.mContentFile = iter->second;
            }

            // Search for the reference. It might no longer exist if its content file was removed.
            Ptr movedRef = searchViaRefNum(refnum);
            if (movedRef.isEmpty())
            {
                Log(Debug::Warning) << "Warning: Dropping moved ref tag for " << refnum.mIndex << " (moved object no longer exists)";
                continue;
            }

            CellStore* otherCell = callback->getCellStore(movedTo);

            if (otherCell == nullptr)
            {
                Log(Debug::Warning) << "Warning: Dropping moved ref tag for " << movedRef.getCellRef().getRefId()
                                    << " (target cell " << movedTo.mWorldspace << " no longer exists). Reference moved back to its original location.";
                // Note by dropping tag the object will automatically re-appear in its original cell, though potentially at inapproriate coordinates.
                // Restore original coordinates:
                movedRef.getRefData().setPosition(movedRef.getCellRef().getPosition());
                continue;
            }

            if (otherCell == this)
            {
                // Should never happen unless someone's tampering with files.
                Log(Debug::Warning) << "Found invalid moved ref, ignoring";
                continue;
            }

            moveTo(movedRef, otherCell);
        }
    }

    bool operator== (const CellStore& left, const CellStore& right)
    {
        return left.getCell()->getCellId()==right.getCell()->getCellId();
    }

    bool operator!= (const CellStore& left, const CellStore& right)
    {
        return !(left==right);
    }

    void CellStore::setFog(ESM::FogState *fog)
    {
        mFogState.reset(fog);
    }

    ESM::FogState* CellStore::getFog() const
    {
        return mFogState.get();
    }

    void clearCorpse(const MWWorld::Ptr& ptr)
    {
        const MWMechanics::CreatureStats& creatureStats = ptr.getClass().getCreatureStats(ptr);
        static const float fCorpseClearDelay = MWBase::Environment::get().getWorld()->getStore().get<ESM::GameSetting>().find("fCorpseClearDelay")->mValue.getFloat();
        if (creatureStats.isDead() &&
            creatureStats.isDeathAnimationFinished() &&
            !ptr.getClass().isPersistent(ptr) &&
            creatureStats.getTimeOfDeath() + fCorpseClearDelay <= MWBase::Environment::get().getWorld()->getTimeStamp())
        {
            MWBase::Environment::get().getWorld()->deleteObject(ptr);
        }
    }

    void CellStore::rest(double hours)
    {
        if (mState == State_Loaded)
        {
            for (CellRefList<ESM::Creature>::List::iterator it (mCreatures.mList.begin()); it!=mCreatures.mList.end(); ++it)
            {
                Ptr ptr = getCurrentPtr(&*it);
                if (!ptr.isEmpty() && ptr.getRefData().getCount() > 0)
                {
                    MWBase::Environment::get().getMechanicsManager()->restoreDynamicStats(ptr, hours, true);
                }
            }
            for (CellRefList<ESM::NPC>::List::iterator it (mNpcs.mList.begin()); it!=mNpcs.mList.end(); ++it)
            {
                Ptr ptr = getCurrentPtr(&*it);
                if (!ptr.isEmpty() && ptr.getRefData().getCount() > 0)
                {
                    MWBase::Environment::get().getMechanicsManager()->restoreDynamicStats(ptr, hours, true);
                }
            }
        }
    }

    void CellStore::recharge(float duration)
    {
        if (duration <= 0)
            return;

        if (mState == State_Loaded)
        {
            for (CellRefList<ESM::Creature>::List::iterator it (mCreatures.mList.begin()); it!=mCreatures.mList.end(); ++it)
            {
                Ptr ptr = getCurrentPtr(&*it);
                if (!ptr.isEmpty() && ptr.getRefData().getCount() > 0)
                {
                    ptr.getClass().getContainerStore(ptr).rechargeItems(duration);
                }
            }
            for (CellRefList<ESM::NPC>::List::iterator it (mNpcs.mList.begin()); it!=mNpcs.mList.end(); ++it)
            {
                Ptr ptr = getCurrentPtr(&*it);
                if (!ptr.isEmpty() && ptr.getRefData().getCount() > 0)
                {
                    ptr.getClass().getContainerStore(ptr).rechargeItems(duration);
                }
            }
            for (CellRefList<ESM::Container>::List::iterator it (mContainers.mList.begin()); it!=mContainers.mList.end(); ++it)
            {
                Ptr ptr = getCurrentPtr(&*it);
                if (!ptr.isEmpty() && ptr.getRefData().getCustomData() != nullptr && ptr.getRefData().getCount() > 0
                && ptr.getClass().getContainerStore(ptr).isResolved())
                {
                    ptr.getClass().getContainerStore(ptr).rechargeItems(duration);
                }
            }

            rechargeItems(duration);
        }
    }

    void CellStore::respawn()
    {
        if (mState == State_Loaded)
        {
            static const int iMonthsToRespawn = MWBase::Environment::get().getWorld()->getStore().get<ESM::GameSetting>().find("iMonthsToRespawn")->mValue.getInteger();
            if (MWBase::Environment::get().getWorld()->getTimeStamp() - mLastRespawn > 24*30*iMonthsToRespawn)
            {
                mLastRespawn = MWBase::Environment::get().getWorld()->getTimeStamp();
                for (CellRefList<ESM::Container>::List::iterator it (mContainers.mList.begin()); it!=mContainers.mList.end(); ++it)
                {
                    Ptr ptr = getCurrentPtr(&*it);
                    ptr.getClass().respawn(ptr);
                }
            }

            for (CellRefList<ESM::Creature>::List::iterator it (mCreatures.mList.begin()); it!=mCreatures.mList.end(); ++it)
            {
                Ptr ptr = getCurrentPtr(&*it);
                clearCorpse(ptr);
                ptr.getClass().respawn(ptr);
            }
            for (CellRefList<ESM::NPC>::List::iterator it (mNpcs.mList.begin()); it!=mNpcs.mList.end(); ++it)
            {
                Ptr ptr = getCurrentPtr(&*it);
                clearCorpse(ptr);
                ptr.getClass().respawn(ptr);
            }
            for (CellRefList<ESM::CreatureLevList>::List::iterator it (mCreatureLists.mList.begin()); it!=mCreatureLists.mList.end(); ++it)
            {
                Ptr ptr = getCurrentPtr(&*it);
                // no need to clearCorpse, handled as part of mCreatures
                ptr.getClass().respawn(ptr);
            }
        }
    }

    void MWWorld::CellStore::rechargeItems(float duration)
    {
        if (!mRechargingItemsUpToDate)
        {
            updateRechargingItems();
            mRechargingItemsUpToDate = true;
        }
        for (const auto& [item, charge] : mRechargingItems)
        {
            MWMechanics::rechargeItem(item, charge, duration);
        }
    }

    void MWWorld::CellStore::updateRechargingItems()
    {
        mRechargingItems.clear();

        const auto update = [this](auto& list)
        {
            for (auto & item : list)
            {
                Ptr ptr = getCurrentPtr(&item);
                if (!ptr.isEmpty() && ptr.getRefData().getCount() > 0)
                {
                    checkItem(ptr);
                }
            }
        };

        update(mWeapons.mList);
        update(mArmors.mList);
        update(mClothes.mList);
        update(mBooks.mList);
    }

    void MWWorld::CellStore::checkItem(const Ptr& ptr)
    {
        if (ptr.getClass().getEnchantment(ptr).empty())
            return;

        std::string enchantmentId = ptr.getClass().getEnchantment(ptr);
        const ESM::Enchantment* enchantment = MWBase::Environment::get().getWorld()->getStore().get<ESM::Enchantment>().search(enchantmentId);
        if (!enchantment)
        {
            Log(Debug::Warning) << "Warning: Can't find enchantment '" << enchantmentId << "' on item " << ptr.getCellRef().getRefId();
            return;
        }

        if (enchantment->mData.mType == ESM::Enchantment::WhenUsed
                || enchantment->mData.mType == ESM::Enchantment::WhenStrikes)
            mRechargingItems.emplace_back(ptr.getBase(), static_cast<float>(enchantment->mData.mCharge));
    }
}
