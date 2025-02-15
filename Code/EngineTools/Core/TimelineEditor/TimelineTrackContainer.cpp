#include "TimelineTrackContainer.h"
#include "System/Serialization/TypeSerialization.h"

//-------------------------------------------------------------------------

namespace EE::Timeline
{
    TEvent<TrackContainer*> TrackContainer::s_onEndModification;
    TEvent<TrackContainer*> TrackContainer::s_onBeginModification;

    //-------------------------------------------------------------------------

    void TrackContainer::Reset()
    {
        for ( auto pTrack : m_tracks )
        {
            EE::Delete( pTrack );
        }
        m_tracks.clear();
        m_isDirty = false;
    }

    Track* TrackContainer::GetTrackForItem( TrackItem const* pItem )
    {
        for ( auto pTrack : m_tracks )
        {
            if ( pTrack->Contains( pItem ) )
            {
                return pTrack;
            }
        }

        return nullptr;
    }

    Track const* TrackContainer::GetTrackForItem( TrackItem const* pItem ) const
    {
        return const_cast<TrackContainer*>( this )->GetTrackForItem( pItem );
    }

    bool TrackContainer::Contains( Track const* pTrack ) const
    {
        return eastl::find( m_tracks.begin(), m_tracks.end(), pTrack ) != m_tracks.end();
    }

    bool TrackContainer::Contains( TrackItem const* pItem ) const
    {
        return GetTrackForItem( pItem ) != nullptr;
    }

    //-------------------------------------------------------------------------

    Track* TrackContainer::CreateTrack( TypeSystem::TypeInfo const* pTrackTypeInfo )
    {
        EE_ASSERT( pTrackTypeInfo->IsDerivedFrom( Track::GetStaticTypeID() ) );

        BeginModification();
        auto pCreatedTrack = Cast<Track>( pTrackTypeInfo->CreateType() );
        m_tracks.emplace_back( pCreatedTrack );
        EndModification();

        return pCreatedTrack;
    }

    void TrackContainer::DeleteTrack( Track* pTrack )
    {
        EE_ASSERT( Contains( pTrack ) );

        BeginModification();
        m_tracks.erase_first( pTrack );
        EE::Delete( pTrack );
        EndModification();
    }

    void TrackContainer::CreateItem( Track* pTrack, float itemStartTime )
    {
        EE_ASSERT( pTrack != nullptr );
        EE_ASSERT( Contains( pTrack ) );

        BeginModification();
        pTrack->CreateItem( itemStartTime );
        EndModification();
    }

    void TrackContainer::UpdateItemTimeRange( TrackItem* pItem, FloatRange const& newTimeRange )
    {
        EE_ASSERT( pItem != nullptr );
        EE_ASSERT( Contains( pItem ) );
        EE_ASSERT( newTimeRange.IsSetAndValid() );

        BeginModification();
        pItem->SetTimeRange( newTimeRange );
        EndModification();
    }

    void TrackContainer::DeleteItem( TrackItem* pItem )
    {
        EE_ASSERT( pItem != nullptr );
        EE_ASSERT( Contains( pItem ) );

        BeginModification();

        for ( auto pTrack : m_tracks )
        {
            if ( pTrack->DeleteItem( pItem ) )
            {
                break;
            }
        }

        EndModification();
    }

    //-------------------------------------------------------------------------

    void TrackContainer::BeginModification()
    {
        if ( m_beginModificationCallCount == 0 )
        {
            if ( s_onBeginModification.HasBoundUsers() )
            {
                s_onBeginModification.Execute( this );
            }
        }
        m_beginModificationCallCount++;
    }

    void TrackContainer::EndModification()
    {
        EE_ASSERT( m_beginModificationCallCount > 0 );
        m_beginModificationCallCount--;

        if ( m_beginModificationCallCount == 0 )
        {
            if ( s_onEndModification.HasBoundUsers() )
            {
                s_onEndModification.Execute( this );
            }
        }

        m_isDirty = true;
    }

    //-------------------------------------------------------------------------

    bool TrackContainer::IsDirty() const
    {
        if ( m_isDirty )
        {
            return true;
        }

        for ( auto const pTrack : m_tracks )
        {
            if ( pTrack->IsDirty() )
            {
                return true;
            }
        }

        return false;
    }

    void TrackContainer::ClearDirtyFlags()
    {
        m_isDirty = false;

        for ( auto pTrack : m_tracks )
        {
            pTrack->ClearDirtyFlags();
        }
    }

    //-------------------------------------------------------------------------

    bool TrackContainer::Serialize( TypeSystem::TypeRegistry const& typeRegistry, Serialization::JsonValue const& dataObjectValue )
    {
        auto FreeTrackData = [this] ()
        {
            for ( auto pTrack : m_tracks )
            {
                EE::Delete( pTrack );
            }
            m_tracks.clear();
        };

        FreeTrackData();

        //-------------------------------------------------------------------------

        if ( !dataObjectValue.IsArray() )
        {
            return false;
        }

        //-------------------------------------------------------------------------

        bool failed = false;

        for ( auto const& trackObjectValue : dataObjectValue.GetArray() )
        {
            auto trackDataIter = trackObjectValue.FindMember( "Track" );
            if ( trackDataIter == trackObjectValue.MemberEnd() )
            {
                failed = true;
                break;
            }

            // Create track
            Track* pTrack = Serialization::CreateAndReadNativeType<Track>( typeRegistry, trackDataIter->value );
            m_tracks.emplace_back( pTrack );

            // Custom serialization
            pTrack->SerializeCustom( typeRegistry, trackObjectValue );

            //-------------------------------------------------------------------------

            auto itemArrayIter = trackObjectValue.FindMember( "Items" );
            if ( itemArrayIter == trackObjectValue.MemberEnd() || !itemArrayIter->value.IsArray() )
            {
                failed = true;
                break;
            }

            //-------------------------------------------------------------------------

            // Deserialize items
            for ( auto const& itemObjectValue : itemArrayIter->value.GetArray() )
            {
                auto itemDataIter = itemObjectValue.FindMember( "Item" );
                if ( itemDataIter == itemObjectValue.MemberEnd() )
                {
                    failed = true;
                    break;
                }

                // Create item
                TrackItem* pItem = Serialization::CreateAndReadNativeType<TrackItem>( typeRegistry, itemDataIter->value );
                pItem->SerializeCustom( typeRegistry, itemObjectValue );
                pTrack->m_items.emplace_back( pItem );
            }

            if ( failed )
            {
                break;
            }
        }

        //-------------------------------------------------------------------------

        if ( failed )
        {
            FreeTrackData();
            m_isDirty = false;
            return false;
        }

        ClearDirtyFlags();
        return true;
    }

    void TrackContainer::Serialize( TypeSystem::TypeRegistry const& typeRegistry, Serialization::JsonWriter& writer )
    {
        writer.StartArray();

        //-------------------------------------------------------------------------

        for ( auto pTrack : m_tracks )
        {
            writer.StartObject();

            // Track
            //-------------------------------------------------------------------------

            writer.Key( "Track" );
            Serialization::WriteNativeType( typeRegistry, pTrack, writer );

            pTrack->SerializeCustom( typeRegistry, writer );

            // Items
            //-------------------------------------------------------------------------

            writer.Key( "Items" );
            writer.StartArray();
            {
                for ( auto pItem : pTrack->m_items )
                {
                    writer.StartObject();
                    {
                        writer.Key( "Item" );
                        Serialization::WriteNativeType( typeRegistry, pItem, writer );
                        pItem->SerializeCustom( typeRegistry, writer );
                    }
                    writer.EndObject();
                }
            }
            writer.EndArray();

            //-------------------------------------------------------------------------

            writer.EndObject();
        }

        //-------------------------------------------------------------------------

        writer.EndArray();
        ClearDirtyFlags();
    }
}