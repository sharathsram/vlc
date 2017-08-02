/*****************************************************************************
 * matroska_segment.cpp : matroska demuxer
 *****************************************************************************
 * Copyright (C) 2003-2010 VLC authors and VideoLAN
 * $Id$
 *
 * Authors: Laurent Aimar <fenrir@via.ecp.fr>
 *          Steve Lhomme <steve.lhomme@free.fr>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#include "matroska_segment.hpp"
#include "chapters.hpp"
#include "demux.hpp"
#include "util.hpp"
#include "Ebml_parser.hpp"
#include "Ebml_dispatcher.hpp"

#include <new>

matroska_segment_c::matroska_segment_c( demux_sys_t & demuxer, EbmlStream & estream )
    :segment(NULL)
    ,es(estream)
    ,i_timescale(MKVD_TIMECODESCALE)
    ,i_duration(-1)
    ,i_mk_start_time(0)
    ,i_seekhead_count(0)
    ,i_seekhead_position(-1)
    ,i_cues_position(-1)
    ,i_tracks_position(-1)
    ,i_info_position(-1)
    ,i_chapters_position(-1)
    ,i_attachments_position(-1)
    ,cluster(NULL)
    ,i_block_pos(0)
    ,p_segment_uid(NULL)
    ,p_prev_segment_uid(NULL)
    ,p_next_segment_uid(NULL)
    ,b_cues(false)
    ,psz_muxing_application(NULL)
    ,psz_writing_application(NULL)
    ,psz_segment_filename(NULL)
    ,psz_title(NULL)
    ,psz_date_utc(NULL)
    ,i_default_edition(0)
    ,sys(demuxer)
    ,ep(NULL)
    ,b_preloaded(false)
    ,b_ref_external_segments(false)
{
}

matroska_segment_c::~matroska_segment_c()
{
    free( psz_writing_application );
    free( psz_muxing_application );
    free( psz_segment_filename );
    free( psz_title );
    free( psz_date_utc );

    delete ep;
    delete segment;
    delete p_segment_uid;
    delete p_prev_segment_uid;
    delete p_next_segment_uid;

    vlc_delete_all( stored_editions );
    vlc_delete_all( translations );
    vlc_delete_all( families );
}


/*****************************************************************************
 * Tools                                                                     *
 *****************************************************************************
 *  * LoadCues : load the cues element and update index
 *  * LoadTags : load ... the tags element
 *  * InformationCreate : create all information, load tags if present
 *****************************************************************************/
void matroska_segment_c::LoadCues( KaxCues *cues )
{
    EbmlElement *el;

    if( b_cues )
    {
        msg_Err( &sys.demuxer, "There can be only 1 Cues per section." );
        return;
    }

    EbmlParser eparser (&es, cues, &sys.demuxer, var_InheritBool( &sys.demuxer, "mkv-use-dummy" ) );
    while( ( el = eparser.Get() ) != NULL )
    {
        if( MKV_IS_ID( el, KaxCuePoint ) )
        {
            uint64_t cue_position = -1;
            mtime_t  cue_mk_time = -1;

            unsigned int track_id = 0;
            bool b_invalid_cue = false;

            eparser.Down();
            while( ( el = eparser.Get() ) != NULL )
            {
                if ( MKV_CHECKED_PTR_DECL( kct_ptr, KaxCueTime, el ) )
                {
                    try
                    {
                        if( unlikely( !kct_ptr->ValidateSize() ) )
                        {
                            msg_Err( &sys.demuxer, "CueTime size too big");
                            b_invalid_cue = true;
                            break;
                        }
                        kct_ptr->ReadData( es.I_O() );
                    }
                    catch(...)
                    {
                        msg_Err( &sys.demuxer, "Error while reading CueTime" );
                        b_invalid_cue = true;
                        break;
                    }
                    cue_mk_time = static_cast<uint64>( *kct_ptr ) * i_timescale / INT64_C(1000);
                }
                else if( MKV_IS_ID( el, KaxCueTrackPositions ) )
                {
                    eparser.Down();
                    try
                    {
                        while( ( el = eparser.Get() ) != NULL )
                        {
                            if( unlikely( !el->ValidateSize() ) )
                            {
                                eparser.Up();
                                msg_Err( &sys.demuxer, "Error %s too big, aborting", typeid(*el).name() );
                                b_invalid_cue = true;
                                break;
                            }

                            if( MKV_CHECKED_PTR_DECL ( kct_ptr, KaxCueTrack, el ) )
                            {
                                kct_ptr->ReadData( es.I_O() );
                                track_id = static_cast<uint16>( *kct_ptr );
                            }
                            else if( MKV_CHECKED_PTR_DECL ( kccp_ptr, KaxCueClusterPosition, el ) )
                            {
                                kccp_ptr->ReadData( es.I_O() );
                                cue_position = segment->GetGlobalPosition( static_cast<uint64>( *kccp_ptr ) );

                                _seeker.add_cluster_position( cue_position );
                            }
                            else if( MKV_CHECKED_PTR_DECL ( kcbn_ptr, KaxCueBlockNumber, el ) )
                            {
                                VLC_UNUSED( kcbn_ptr );
                            }
#if LIBMATROSKA_VERSION >= 0x010401
                            else if( MKV_IS_ID( el, KaxCueRelativePosition ) )
                            {
                                b_invalid_cue = true; // since we do not support this type of cue: IGNORE
                            }
                            else if( MKV_IS_ID( el, KaxCueBlockNumber ) )
                            {
                                b_invalid_cue = true; // since we do not support this type of cue: IGNORE
                            }
                            else if( MKV_IS_ID( el, KaxCueReference ) )
                            {
                                b_invalid_cue = true; // since we do not support this type of cue: IGNORE
                            }
                            else if( MKV_IS_ID( el, KaxCueDuration ) )
                            {
                                /* For future use */
                            }
#endif
                            else
                            {
                                msg_Dbg( &sys.demuxer, "         * Unknown (%s)", typeid(*el).name() );
                            }
                        }
                    }
                    catch(...)
                    {
                        eparser.Up();
                        msg_Err( &sys.demuxer, "Error while reading %s", typeid(*el).name() );
                        b_invalid_cue = true;
                        break;
                    }
                    eparser.Up();
                }
                else
                {
                    msg_Dbg( &sys.demuxer, "     * Unknown (%s)", typeid(*el).name() );
                }
            }
            eparser.Up();

            if( track_id != 0 && cue_mk_time != -1 && cue_position != static_cast<uint64_t>( -1 ) ) {

                if( tracks.find( track_id ) != tracks.end() )
                {
                    int level = SegmentSeeker::Seekpoint::DISABLED;

                    if( ! b_invalid_cue )
                    {
                        level = SegmentSeeker::Seekpoint::QUESTIONABLE; // TODO: var_InheritBool( ..., "mkv-trust-cues" );
                    }

                    _seeker.add_seekpoint( track_id, level, cue_position, cue_mk_time );
                }
                else
                    msg_Warn( &sys.demuxer, "Found cue with invalid track id = %u", track_id );
            }
        }
        else
        {
            msg_Dbg( &sys.demuxer, " * Unknown (%s)", typeid(*el).name() );
        }
    }
    b_cues = true;
    msg_Dbg( &sys.demuxer, "|   - loading cues done." );
}


static const struct {
    vlc_meta_type_t type;
    const char *key;
    int target_type; /* 0 is valid for all target_type */
} metadata_map[] = {
                     {vlc_meta_Album,       "TITLE",         50},
                     {vlc_meta_Title,       "TITLE",         0},
                     {vlc_meta_Artist,      "ARTIST",        0},
                     {vlc_meta_Genre,       "GENRE",         0},
                     {vlc_meta_Copyright,   "COPYRIGHT",     0},
                     {vlc_meta_TrackNumber, "PART_NUMBER",   0},
                     {vlc_meta_Description, "DESCRIPTION",   0},
                     {vlc_meta_Description, "COMMENT",       0},
                     {vlc_meta_Rating,      "RATING",        0},
                     {vlc_meta_Date,        "DATE_RELEASED", 0},
                     {vlc_meta_Date,        "DATE_RELEASE",  0},
                     {vlc_meta_Date,        "DATE_RECORDED", 0},
                     {vlc_meta_URL,         "URL",           0},
                     {vlc_meta_Publisher,   "PUBLISHER",     0},
                     {vlc_meta_EncodedBy,   "ENCODED_BY",    0},
                     {vlc_meta_TrackTotal,  "TOTAL_PARTS",   0},
                     {vlc_meta_Album,       "ALBUM",         0},
                     {vlc_meta_Title,       NULL,            0},
};

bool matroska_segment_c::ParseSimpleTags( SimpleTag* pout_simple, KaxTagSimple *tag, int target_type )
{
    EbmlParser eparser ( &es, tag, &sys.demuxer, var_InheritBool( &sys.demuxer, "mkv-use-dummy" ) );
    EbmlElement *el;
    size_t max_size = tag->GetSize();
    size_t size = 0;

    if( !sys.meta )
        sys.meta = vlc_meta_New();

    msg_Dbg( &sys.demuxer, "|   + Simple Tag ");
    try
    {
        while( ( el = eparser.Get() ) != NULL && size < max_size)
        {
            if( unlikely( !el->ValidateSize() ) )
            {
                msg_Err( &sys.demuxer, "Error %s too big ignoring the tag", typeid(*el).name() );
                delete ep;
                return false;
            }
            if( MKV_CHECKED_PTR_DECL ( ktn_ptr, KaxTagName, el ) )
            {
                ktn_ptr->ReadData( es.I_O(), SCOPE_ALL_DATA );
                pout_simple->tag_name = UTFstring( *ktn_ptr ).GetUTF8().c_str();
            }
            else if( MKV_CHECKED_PTR_DECL ( kts_ptr, KaxTagString, el ) )
            {
                kts_ptr->ReadData( es.I_O(), SCOPE_ALL_DATA );
                pout_simple->value = UTFstring( *kts_ptr ).GetUTF8().c_str();
            }
            else if(  MKV_CHECKED_PTR_DECL ( ktl_ptr, KaxTagLangue, el ) )
            {
                ktl_ptr->ReadData( es.I_O(), SCOPE_ALL_DATA );
                pout_simple->lang = *ktl_ptr;
            }
            else if(  MKV_CHECKED_PTR_DECL ( ktd_ptr, KaxTagDefault, el ) )
            {
                VLC_UNUSED(ktd_ptr); // TODO: we do not care about this value, but maybe we should?
            }
            /*Tags can be nested*/
            else if( MKV_CHECKED_PTR_DECL ( kts_ptr, KaxTagSimple, el) )
            {
                SimpleTag st; // ParseSimpleTags will write to this variable
                              // the SimpleTag is valid if ParseSimpleTags returns `true`

                if (ParseSimpleTags( &st, kts_ptr, target_type )) {
                  pout_simple->sub_tags.push_back( st );
                }
            }
            /*TODO Handle binary tags*/
            size += el->HeadSize() + el->GetSize();
        }
    }
    catch(...)
    {
        msg_Err( &sys.demuxer, "Error while reading Tag ");
        delete ep;
        return false;
    }

    if( pout_simple->tag_name.empty() )
    {
        msg_Warn( &sys.demuxer, "Invalid MKV SimpleTag found.");
        return false;
    }
    for( int i = 0; metadata_map[i].key; i++ )
    {
        if( pout_simple->tag_name == metadata_map[i].key &&
            (metadata_map[i].target_type == 0 || target_type == metadata_map[i].target_type ) )
        {
            vlc_meta_Set( sys.meta, metadata_map[i].type, pout_simple->value.c_str () );
            msg_Dbg( &sys.demuxer, "|   |   + Meta %s: %s", pout_simple->tag_name.c_str (), pout_simple->value.c_str ());
            goto done;
        }
    }
    msg_Dbg( &sys.demuxer, "|   |   + Meta %s: %s", pout_simple->tag_name.c_str (), pout_simple->value.c_str ());
    vlc_meta_AddExtra( sys.meta, pout_simple->tag_name.c_str (), pout_simple->value.c_str ());
done:
    return true;
}

void matroska_segment_c::LoadTags( KaxTags *tags )
{
    /* Master elements */
    EbmlParser eparser = EbmlParser( &es, tags, &sys.demuxer, true );
    EbmlElement *el;

    while( ( el = eparser.Get() ) != NULL )
    {
        if( MKV_IS_ID( el, KaxTag ) )
        {
            Tag tag;

            msg_Dbg( &sys.demuxer, "+ Tag" );
            eparser.Down();
            int target_type = 50;
            while( ( el = eparser.Get() ) != NULL )
            {
                if( MKV_IS_ID( el, KaxTagTargets ) )
                {
                    msg_Dbg( &sys.demuxer, "|   + Targets" );
                    eparser.Down();
                    while( ( el = eparser.Get() ) != NULL )
                    {
                        try
                        {
                            if( unlikely( !el->ValidateSize() ) )
                            {
                                msg_Err( &sys.demuxer, "Invalid size while reading tag");
                                break;
                            }
                            if( MKV_CHECKED_PTR_DECL ( ktttv_ptr, KaxTagTargetTypeValue, el ) )
                            {
                                ktttv_ptr->ReadData( es.I_O() );

                                msg_Dbg( &sys.demuxer, "|   |   + TargetTypeValue: %u", uint32(*ktttv_ptr));
                                target_type = static_cast<uint32>( *ktttv_ptr );
                            }
                            else if( MKV_CHECKED_PTR_DECL ( kttu_ptr, KaxTagTrackUID, el ) )
                            {
                                tag.i_tag_type = TRACK_UID;
                                kttu_ptr->ReadData( es.I_O() );
                                tag.i_uid = static_cast<uint64>( *kttu_ptr );
                                msg_Dbg( &sys.demuxer, "|   |   + TrackUID: %" PRIu64, tag.i_uid);

                            }
                            else if( MKV_CHECKED_PTR_DECL ( kteu_ptr, KaxTagEditionUID, el ) )
                            {
                                tag.i_tag_type = EDITION_UID;
                                kteu_ptr->ReadData( es.I_O() );
                                tag.i_uid = static_cast<uint64>( *kteu_ptr );
                                msg_Dbg( &sys.demuxer, "|   |   + EditionUID: %" PRIu64, tag.i_uid);
                            }
                            else if( MKV_CHECKED_PTR_DECL ( ktcu_ptr, KaxTagChapterUID, el ) )
                            {
                                tag.i_tag_type = CHAPTER_UID;
                                ktcu_ptr->ReadData( es.I_O() );
                                tag.i_uid = static_cast<uint64>( *ktcu_ptr );
                                msg_Dbg( &sys.demuxer, "|   |   + ChapterUID: %" PRIu64, tag.i_uid);
                            }
                            else if( MKV_CHECKED_PTR_DECL ( ktau_ptr, KaxTagAttachmentUID, el ) )
                            {
                                tag.i_tag_type = ATTACHMENT_UID;
                                ktau_ptr->ReadData( es.I_O() );
                                tag.i_uid = static_cast<uint64>( *ktau_ptr );
                                msg_Dbg( &sys.demuxer, "|   |   + AttachmentUID: %" PRIu64, tag.i_uid);
                            }
                            else
                            {
                                msg_Dbg( &sys.demuxer, "|   |   + LoadTag Unknown (%s)", typeid( *el ).name() );
                            }
                        }
                        catch(...)
                        {
                            msg_Err( &sys.demuxer, "Error while reading tag");
                            break;
                        }
                    }
                    eparser.Up();
                }
                else if( MKV_CHECKED_PTR_DECL ( kts_ptr, KaxTagSimple, el ) )
                {
                    SimpleTag simple;

                    if (ParseSimpleTags(&simple, kts_ptr, target_type )) {
                      tag.simple_tags.push_back( simple );
                    }
                }
                else
                {
                    msg_Dbg( &sys.demuxer, "|   + LoadTag Unknown (%s)", typeid( *el ).name() );
                }
            }
            eparser.Up();
            this->tags.push_back(tag);
        }
        else
        {
            msg_Dbg( &sys.demuxer, "+ Unknown (%s)", typeid( *el ).name() );
        }
    }

    msg_Dbg( &sys.demuxer, "loading tags done." );
}

/*****************************************************************************
 * InformationCreate:
 *****************************************************************************/
void matroska_segment_c::InformationCreate( )
{
    if( !sys.meta )
        sys.meta = vlc_meta_New();

    if( psz_title )
    {
        vlc_meta_SetTitle( sys.meta, psz_title );
    }
}


/*****************************************************************************
 * Misc
 *****************************************************************************/

void matroska_segment_c::IndexAppendCluster( KaxCluster *cluster )
{
    _seeker.add_cluster( cluster );
}

bool matroska_segment_c::PreloadClusters(uint64 i_cluster_pos)
{
    struct ClusterHandlerPayload
    {
        matroska_segment_c * const obj;
        bool stop_parsing;

    } payload = { this, false };

    MKV_SWITCH_CREATE(EbmlTypeDispatcher, ClusterHandler, ClusterHandlerPayload )
    {
        MKV_SWITCH_INIT();

        E_CASE( KaxCluster, kcluster )
        {
            if( vars.obj->ParseCluster( &kcluster, false ) )
                vars.obj->IndexAppendCluster( &kcluster );
        }

        E_CASE_DEFAULT( el )
        {
            VLC_UNUSED( el );
            vars.stop_parsing = true;
        }
    };

    {
        es.I_O().setFilePointer( i_cluster_pos );

        while (payload.stop_parsing == false)
        {
            EbmlParser parser ( &es, segment, &sys.demuxer, var_InheritBool( &sys.demuxer, "mkv-use-dummy" ) );
            EbmlElement* el = parser.Get();

            if( el == NULL )
                break;

            ClusterHandler::Dispatcher().send( el, ClusterHandler::Payload( payload ) );
        }
    }

    return true;
}

bool matroska_segment_c::PreloadFamily( const matroska_segment_c & of_segment )
{
    if ( b_preloaded )
        return false;

    if ( SameFamily( of_segment ) )
        return Preload( );

    return false;
}

bool matroska_segment_c::CompareSegmentUIDs( const matroska_segment_c * p_item_a, const matroska_segment_c * p_item_b )
{
    EbmlBinary *p_tmp;

    if ( p_item_a == NULL || p_item_b == NULL )
        return false;

    p_tmp = static_cast<EbmlBinary *>( p_item_a->p_segment_uid );
    if ( p_item_b->p_prev_segment_uid != NULL
          && *p_tmp == *p_item_b->p_prev_segment_uid )
        return true;

    p_tmp = static_cast<EbmlBinary *>( p_item_a->p_next_segment_uid );
    if ( !p_tmp )
        return false;

    if ( p_item_b->p_segment_uid != NULL
          && *p_tmp == *p_item_b->p_segment_uid )
        return true;

    if ( p_item_b->p_prev_segment_uid != NULL
          && *p_tmp == *p_item_b->p_prev_segment_uid )
        return true;

    return false;
}

bool matroska_segment_c::SameFamily( const matroska_segment_c & of_segment ) const
{
    for (size_t i=0; i<families.size(); i++)
    {
        for (size_t j=0; j<of_segment.families.size(); j++)
        {
            if ( *(families[i]) == *(of_segment.families[j]) )
                return true;
        }
    }
    return false;
}

bool matroska_segment_c::Preload( )
{
    if ( b_preloaded )
        return false;

    EbmlElement *el = NULL;

    ep->Reset( &sys.demuxer );

    while( ( el = ep->Get() ) != NULL )
    {
        if( MKV_IS_ID( el, KaxSeekHead ) )
        {
            /* Multiple allowed */
            /* We bail at 10, to prevent possible recursion */
            msg_Dbg(  &sys.demuxer, "|   + Seek head" );
            if( i_seekhead_count < 10 )
            {
                i_seekhead_position = el->GetElementPosition();
                ParseSeekHead( static_cast<KaxSeekHead*>( el ) );
            }
        }
        else if( MKV_IS_ID( el, KaxInfo ) )
        {
            /* Multiple allowed, mandatory */
            msg_Dbg(  &sys.demuxer, "|   + Information" );
            if( i_info_position < 0 )
            {
                ParseInfo( static_cast<KaxInfo*>( el ) );
                i_info_position = el->GetElementPosition();
            }
        }
        else if( MKV_CHECKED_PTR_DECL ( kt_ptr, KaxTracks, el ) )
        {
            /* Multiple allowed */
            msg_Dbg(  &sys.demuxer, "|   + Tracks" );
            if( i_tracks_position < 0 )
            {
                ParseTracks( kt_ptr );
            }
            if ( tracks.size() == 0 )
            {
                msg_Err( &sys.demuxer, "No tracks supported" );
                return false;
            }
            i_tracks_position = el->GetElementPosition();
        }
        else if( MKV_CHECKED_PTR_DECL ( kc_ptr, KaxCues, el ) )
        {
            msg_Dbg(  &sys.demuxer, "|   + Cues" );
            if( i_cues_position < 0 )
            {
                LoadCues( kc_ptr );
                i_cues_position = el->GetElementPosition();
            }
        }
        else if( MKV_CHECKED_PTR_DECL ( kc_ptr, KaxCluster, el ) )
        {
            if( var_InheritBool( &sys.demuxer, "mkv-preload-clusters" ) )
            {
                PreloadClusters        ( kc_ptr->GetElementPosition() );
                es.I_O().setFilePointer( kc_ptr->GetElementPosition() );
            }
            msg_Dbg( &sys.demuxer, "|   + Cluster" );

            if( !ParseCluster( kc_ptr ) )
                break;

            cluster = kc_ptr;
            IndexAppendCluster( cluster );

            // add first cluster as trusted seekpoint for all tracks
            for( tracks_map_t::const_iterator it = tracks.begin();
                 it != tracks.end(); ++it )
            {
                _seeker.add_seekpoint( it->first, SegmentSeeker::Seekpoint::TRUSTED,
                  cluster->GetElementPosition(), 0 );
            }

            ep->Down();
            /* stop pre-parsing the stream */
            break;
        }
        else if( MKV_CHECKED_PTR_DECL ( ka_ptr, KaxAttachments, el ) )
        {
            msg_Dbg( &sys.demuxer, "|   + Attachments" );
            if( i_attachments_position < 0 )
            {
                ParseAttachments( ka_ptr );
                i_attachments_position = el->GetElementPosition();
            }
        }
        else if( MKV_CHECKED_PTR_DECL ( kc_ptr, KaxChapters, el ) )
        {
            msg_Dbg( &sys.demuxer, "|   + Chapters" );
            if( i_chapters_position < 0 )
            {
                ParseChapters( kc_ptr );
                i_chapters_position = el->GetElementPosition();
            }
        }
        else if( MKV_CHECKED_PTR_DECL ( kt_ptr, KaxTags, el ) )
        {
            msg_Dbg( &sys.demuxer, "|   + Tags" );
            if(tags.empty ())
            {
                LoadTags( kt_ptr );
            }
        }
        else if( MKV_IS_ID ( el, EbmlVoid ) )
            msg_Dbg( &sys.demuxer, "|   + Void" );
        else
            msg_Dbg( &sys.demuxer, "|   + Preload Unknown (%s)", typeid(*el).name() );
    }

    ComputeTrackPriority();

    b_preloaded = true;

    if( cluster )
        EnsureDuration();

    return true;
}

/* Here we try to load elements that were found in Seek Heads, but not yet parsed */
bool matroska_segment_c::LoadSeekHeadItem( const EbmlCallbacks & ClassInfos, int64_t i_element_position )
{
    int64_t     i_sav_position = static_cast<int64_t>( es.I_O().getFilePointer() );
    EbmlElement *el;

    es.I_O().setFilePointer( i_element_position, seek_beginning );
    el = es.FindNextID( ClassInfos, 0xFFFFFFFFL);

    if( el == NULL )
    {
        msg_Err( &sys.demuxer, "cannot load some cues/chapters/tags etc. (broken seekhead or file)" );
        es.I_O().setFilePointer( i_sav_position, seek_beginning );
        return false;
    }

    if( MKV_CHECKED_PTR_DECL ( ksh_ptr, KaxSeekHead, el ) )
    {
        /* Multiple allowed */
        msg_Dbg( &sys.demuxer, "|   + Seek head" );
        if( i_seekhead_count < 10 )
        {
            if ( i_seekhead_position != i_element_position )
            {
                i_seekhead_position = i_element_position;
                ParseSeekHead( ksh_ptr );
            }
        }
    }
    else if( MKV_CHECKED_PTR_DECL ( ki_ptr, KaxInfo, el ) ) // FIXME
    {
        /* Multiple allowed, mandatory */
        msg_Dbg( &sys.demuxer, "|   + Information" );
        if( i_info_position < 0 )
        {
            ParseInfo( ki_ptr );
            i_info_position = i_element_position;
        }
    }
    else if( MKV_CHECKED_PTR_DECL ( kt_ptr, KaxTracks, el ) ) // FIXME
    {
        /* Multiple allowed */
        msg_Dbg( &sys.demuxer, "|   + Tracks" );
        if( i_tracks_position < 0 )
            ParseTracks( kt_ptr );
        if ( tracks.size() == 0 )
        {
            msg_Err( &sys.demuxer, "No tracks supported" );
            delete el;
            es.I_O().setFilePointer( i_sav_position, seek_beginning );
            return false;
        }
        i_tracks_position = i_element_position;
    }
    else if( MKV_CHECKED_PTR_DECL ( kc_ptr, KaxCues, el ) )
    {
        msg_Dbg( &sys.demuxer, "|   + Cues" );
        if( i_cues_position < 0 )
        {
            LoadCues( kc_ptr );
            i_cues_position = i_element_position;
        }
    }
    else if( MKV_CHECKED_PTR_DECL ( ka_ptr, KaxAttachments, el ) )
    {
        msg_Dbg( &sys.demuxer, "|   + Attachments" );
        if( i_attachments_position < 0 )
        {
            ParseAttachments( ka_ptr );
            i_attachments_position = i_element_position;
        }
    }
    else if( MKV_CHECKED_PTR_DECL ( kc_ptr, KaxChapters, el ) )
    {
        msg_Dbg( &sys.demuxer, "|   + Chapters" );
        if( i_chapters_position < 0 )
        {
            ParseChapters( kc_ptr );
            i_chapters_position = i_element_position;
        }
    }
    else if( MKV_CHECKED_PTR_DECL ( kt_ptr, KaxTags, el ) )
    {
        msg_Dbg( &sys.demuxer, "|   + Tags" );
        if(tags.empty ())
        {
            LoadTags( kt_ptr );
        }
    }
    else
    {
        msg_Dbg( &sys.demuxer, "|   + LoadSeekHeadItem Unknown (%s)", typeid(*el).name() );
    }
    delete el;

    es.I_O().setFilePointer( i_sav_position, seek_beginning );
    return true;
}

bool matroska_segment_c::FastSeek( mtime_t i_mk_date, mtime_t i_mk_time_offset )
{
    if( Seek( i_mk_date, i_mk_time_offset ) )
    {
        sys.i_start_pts = sys.i_pts;
        es_out_Control( sys.demuxer.out, ES_OUT_SET_NEXT_DISPLAY_TIME, sys.i_start_pts );
        return true;
    }
    return false;
}

bool matroska_segment_c::Seek( mtime_t i_absolute_mk_date, mtime_t i_mk_time_offset )
{
    SegmentSeeker::tracks_seekpoint_t seekpoints;

    uint64_t i_seek_position = -1;
    mtime_t i_mk_seek_time   = -1;
    mtime_t i_mk_date = i_absolute_mk_date - i_mk_time_offset;

    // reset information for all tracks //

    for( tracks_map_t::iterator it = tracks.begin(); it != tracks.end(); ++it )
    {
        mkv_track_t &track = *it->second;

        track.i_skip_until_fpos = -1;
        if( track.i_last_dts > VLC_TS_INVALID )
            track.b_discontinuity = true;
        track.i_last_dts        = VLC_TS_INVALID;
    }

    // find appropriate seekpoints //

    try {
        seekpoints = _seeker.get_seekpoints( *this, i_mk_date, priority_tracks );
    }
    catch( std::exception const& e )
    {
        msg_Err( &sys.demuxer, "error during seek: \"%s\", aborting!", e.what() );
        return false;
    }

    // initialize seek information in order to set up playback //

    for( SegmentSeeker::tracks_seekpoint_t::const_iterator it = seekpoints.begin(); it != seekpoints.end(); ++it )
    {
        if( i_seek_position > it->second.fpos )
        {
            i_seek_position = it->second.fpos;
            i_mk_seek_time  = it->second.pts;
        }

        tracks.at( it->first )->i_skip_until_fpos = it->second.fpos;
        tracks.at( it->first )->i_last_dts        = it->second.pts;

        msg_Dbg( &sys.demuxer, "seek: preroll{ track: %u, pts: %" PRId64 ", fpos: %" PRIu64 " } ",
          it->first, it->second.pts, it->second.fpos );
    }

    // propogate seek information //

    sys.i_pcr       = VLC_TS_INVALID;
    sys.i_pts       = VLC_TS_0 + i_mk_seek_time + i_mk_time_offset;
    sys.i_start_pts = VLC_TS_0 + i_absolute_mk_date;

    es_out_Control( sys.demuxer.out, ES_OUT_SET_NEXT_DISPLAY_TIME, sys.i_start_pts );

    // make the jump //

    _seeker.mkv_jump_to( *this, i_seek_position );

    // debug diagnostics //

    msg_Dbg( &sys.demuxer, "seek: preroll{ start-pts: %" PRId64 ", start-fpos: %" PRIu64 "} ",
      sys.i_pts, i_seek_position );

    return true;
}


int matroska_segment_c::FindTrackByBlock(tracks_map_t::iterator* p_track_it,
                                             const KaxBlock *p_block, const KaxSimpleBlock *p_simpleblock )
{
    *p_track_it = tracks.end();

    if( p_block == NULL && p_simpleblock == NULL )
        return VLC_EGENERIC;

    if (p_block != NULL)
    {
        *p_track_it = tracks.find( p_block->TrackNum() );
    }
    else if( p_simpleblock != NULL)
    {
        *p_track_it = tracks.find( p_simpleblock->TrackNum() );
    }

    return *p_track_it != tracks.end() ? VLC_SUCCESS : VLC_EGENERIC;
}

void matroska_segment_c::ComputeTrackPriority()
{
    bool b_has_default_video = false;
    bool b_has_default_audio = false;
    /* check for default */
    for( tracks_map_t::const_iterator it = tracks.begin(); it != tracks.end();
         ++it )
    {
        mkv_track_t &track = *it->second;

        bool flag = track.b_enabled && ( track.b_default || track.b_forced );

        switch( track.fmt.i_cat )
        {
            case VIDEO_ES: b_has_default_video |= flag; break;
            case AUDIO_ES: b_has_default_audio |= flag; break;
            default: break; // ignore
        }
    }

    for( tracks_map_t::iterator it = tracks.begin(); it != tracks.end(); ++it )
    {
        tracks_map_t::key_type track_id = it->first;
        mkv_track_t          & track    = *it->second;

        if( unlikely( track.fmt.i_cat == UNKNOWN_ES || track.codec.empty() ) )
        {
            msg_Warn( &sys.demuxer, "invalid track[%d]", static_cast<int>( track_id ) );
            track.p_es = NULL;
            continue;
        }
        else if( unlikely( !b_has_default_video && track.fmt.i_cat == VIDEO_ES ) )
        {
            track.b_default = true;
            b_has_default_video = true;
        }
        else if( unlikely( !b_has_default_audio &&  track.fmt.i_cat == AUDIO_ES ) )
        {
            track.b_default = true;
            b_has_default_audio = true;
        }
        if( unlikely( !track.b_enabled ) )
            track.fmt.i_priority = ES_PRIORITY_NOT_SELECTABLE;
        else if( track.b_forced )
            track.fmt.i_priority = ES_PRIORITY_SELECTABLE_MIN + 2;
        else if( track.b_default )
            track.fmt.i_priority = ES_PRIORITY_SELECTABLE_MIN + 1;
        else
            track.fmt.i_priority = ES_PRIORITY_SELECTABLE_MIN;

        /* Avoid multivideo tracks when unnecessary */
        if( track.fmt.i_cat == VIDEO_ES )
            track.fmt.i_priority--;
    }

    // find track(s) with highest priority //
    {
        int   score = -1;
        int es_type = -1;

        for( tracks_map_t::const_iterator it = this->tracks.begin(); it != this->tracks.end(); ++it )
        {
            int track_score = -1;

            switch( it->second->fmt.i_cat )
            {
                case VIDEO_ES: ++track_score;
                case AUDIO_ES: ++track_score;
                case   SPU_ES: ++track_score;
                default:
                  if( score < track_score )
                  {
                      es_type = it->second->fmt.i_cat;
                      score   = track_score;
                  }
            }
        }

        for( tracks_map_t::const_iterator it = this->tracks.begin(); it != this->tracks.end(); ++it )
        {
            if( it->second->fmt.i_cat == es_type )
                priority_tracks.push_back( it->first );
        }
    }
}

void matroska_segment_c::EnsureDuration()
{
    if ( i_duration > 0 )
        return;

    i_duration = -1;

    bool b_seekable;

    if( vlc_stream_Control( sys.demuxer.s, STREAM_CAN_FASTSEEK, &b_seekable ) ||
        !b_seekable )
    {
        msg_Warn( &sys.demuxer, "could not look for the segment duration" );
        return;
    }

    uint64 i_current_position = es.I_O().getFilePointer();
    uint64 i_last_cluster_pos = cluster->GetElementPosition();

    // find the last Cluster from the Cues

    if ( b_cues && _seeker._cluster_positions.size() )
        i_last_cluster_pos = *_seeker._cluster_positions.rbegin();
    else if( !cluster->IsFiniteSize() )
        return;

    es.I_O().setFilePointer( i_last_cluster_pos, seek_beginning );

    EbmlParser eparser ( &es, segment, &sys.demuxer, var_InheritBool(
          &sys.demuxer, "mkv-use-dummy" ) );

    // locate the definitely last cluster in the stream

    while( EbmlElement* el = eparser.Get() )
    {
        if( !el->IsFiniteSize() && el->GetElementPosition() != i_last_cluster_pos )
        {
            es.I_O().setFilePointer( i_current_position, seek_beginning );
            return;
        }

        if( MKV_IS_ID( el, KaxCluster ) )
            i_last_cluster_pos = el->GetElementPosition();
    }

    // find the last timecode in the Cluster

    eparser.Reset( &sys.demuxer );
    es.I_O().setFilePointer( i_last_cluster_pos, seek_beginning );

    EbmlElement* el = eparser.Get();
    MKV_CHECKED_PTR_DECL( p_last_cluster, KaxCluster, el );

    if( p_last_cluster &&
        ParseCluster( p_last_cluster, false, SCOPE_PARTIAL_DATA ) )
    {
        // use the last block + duration
        uint64 i_last_timecode = p_last_cluster->GlobalTimecode();
        for( unsigned int i = 0; i < p_last_cluster->ListSize(); i++ )
        {
            EbmlElement *l = (*p_last_cluster)[i];

            if( MKV_CHECKED_PTR_DECL ( block, KaxSimpleBlock, l ) )
            {
                block->SetParent( *p_last_cluster );
                i_last_timecode = std::max(i_last_timecode, block->GlobalTimecode());
            }
            else if( MKV_CHECKED_PTR_DECL ( group, KaxBlockGroup, l ) )
            {
                uint64 i_group_timecode = 0;
                for( unsigned int j = 0; j < group->ListSize(); j++ )
                {
                    EbmlElement *l = (*group)[j];

                    if( MKV_CHECKED_PTR_DECL ( block, KaxBlock, l ) )
                    {
                        block->SetParent( *p_last_cluster );
                        i_group_timecode += block->GlobalTimecode();
                    }
                    else if( MKV_CHECKED_PTR_DECL ( kbd_ptr, KaxBlockDuration, l ) )
                    {
                        i_group_timecode += static_cast<uint64>( *kbd_ptr );
                    }
                }
                i_last_timecode = std::max(i_last_timecode, i_group_timecode);
            }
        }

        i_duration = ( i_last_timecode - cluster->GlobalTimecode() ) / INT64_C(1000000);
        msg_Dbg( &sys.demuxer, " extracted Duration=%" PRId64, i_duration );
    }

    // get back to the reading position we were at before looking for a duration
    es.I_O().setFilePointer( i_current_position, seek_beginning );
}

bool matroska_segment_c::ESCreate()
{
    /* add all es */
    msg_Dbg( &sys.demuxer, "found %d es", static_cast<int>( tracks.size() ) );

    for( tracks_map_t::iterator it = tracks.begin(); it != tracks.end(); ++it )
    {
        tracks_map_t::key_type   track_id = it->first;
        mkv_track_t            & track    = *it->second;

        if( unlikely( track.fmt.i_cat == UNKNOWN_ES || track.codec.empty() ) )
        {
            msg_Warn( &sys.demuxer, "invalid track[%d]", static_cast<int>( track_id ) );
            track.p_es = NULL;
            continue;
        }

        if( !track.p_es )
            track.p_es = es_out_Add( sys.demuxer.out, &track.fmt );

        /* Turn on a subtitles track if it has been flagged as default -
         * but only do this if no subtitles track has already been engaged,
         * either by an earlier 'default track' (??) or by default
         * language choice behaviour.
         */
        if( track.b_default || track.b_forced )
        {
            es_out_Control( sys.demuxer.out, ES_OUT_SET_ES_DEFAULT, track.p_es );
        }
    }

    return true;
}

void matroska_segment_c::ESDestroy( )
{
    sys.p_ev->ResetPci();

    for( tracks_map_t::iterator it = tracks.begin(); it != tracks.end(); ++it )
    {
        mkv_track_t & track = *it->second;

        if( track.p_es != NULL )
        {
            es_out_Del( sys.demuxer.out, track.p_es );
            track.p_es = NULL;
        }
    }
}

int matroska_segment_c::BlockGet( KaxBlock * & pp_block, KaxSimpleBlock * & pp_simpleblock, bool *pb_key_picture, bool *pb_discardable_picture, int64_t *pi_duration )
{
    tracks_map_t::iterator track_it;

    pp_simpleblock = NULL;
    pp_block = NULL;

    *pb_key_picture         = true;
    *pb_discardable_picture = false;
    *pi_duration = 0;

    struct BlockPayload {
        matroska_segment_c * const obj;
        EbmlParser         * const ep;
        demux_t            * const p_demuxer;
        KaxBlock          *& block;
        KaxSimpleBlock    *& simpleblock;

        int64_t            & i_duration;
        bool               & b_key_picture;
        bool               & b_discardable_picture;
        bool                 b_cluster_timecode;

    } payload = {
        this, ep, &sys.demuxer, pp_block, pp_simpleblock,
        *pi_duration, *pb_key_picture, *pb_discardable_picture, true
    };

    MKV_SWITCH_CREATE( EbmlTypeDispatcher, BlockGetHandler_l1, BlockPayload )
    {
        MKV_SWITCH_INIT();

        E_CASE( KaxCluster, kcluster )
        {
            vars.obj->cluster = &kcluster;
            vars.b_cluster_timecode = false;
            vars.ep->Down ();
        }
        E_CASE( KaxCues, kcue )
        {
            VLC_UNUSED( kcue );
            msg_Warn( vars.p_demuxer, "find KaxCues FIXME" );
            throw VLC_EGENERIC;
        }
        E_CASE_DEFAULT(element)
        {
            msg_Dbg( vars.p_demuxer, "Unknown (%s)", typeid (element).name () );
        }
    };

    MKV_SWITCH_CREATE( EbmlTypeDispatcher, BlockGetHandler_l2, BlockPayload )
    {
        MKV_SWITCH_INIT();

        E_CASE( KaxClusterTimecode, ktimecode )
        {
            ktimecode.ReadData( vars.obj->es.I_O(), SCOPE_ALL_DATA );
            vars.obj->cluster->InitTimecode( static_cast<uint64>( ktimecode ), vars.obj->i_timescale );
            vars.obj->IndexAppendCluster( vars.obj->cluster );
            vars.b_cluster_timecode = true;
        }
        E_CASE( KaxClusterSilentTracks, ksilent )
        {
            vars.obj->ep->Down ();

            VLC_UNUSED( ksilent );
        }
        E_CASE( KaxBlockGroup, kbgroup )
        {
            vars.obj->i_block_pos = kbgroup.GetElementPosition();
            vars.obj->ep->Down ();
        }
        E_CASE( KaxSimpleBlock, ksblock )
        {
            if( vars.b_cluster_timecode == false )
            {
                msg_Warn( vars.p_demuxer, "ignoring SimpleBlock prior to mandatory Timecode" );
                return;
            }

            vars.simpleblock = &ksblock;
            vars.simpleblock->ReadData( vars.obj->es.I_O() );
            vars.simpleblock->SetParent( *vars.obj->cluster );

            if( ksblock.IsKeyframe() )
            {
                tracks_map_t::iterator track_it;
                bool const b_valid_track = !vars.obj->FindTrackByBlock( &track_it, NULL, &ksblock );
                if (b_valid_track)
                    vars.obj->_seeker.add_seekpoint( ksblock.TrackNum(), SegmentSeeker::Seekpoint::TRUSTED, ksblock.GetElementPosition(), ksblock.GlobalTimecode() / 1000 );
            }
        }
    };

    MKV_SWITCH_CREATE( EbmlTypeDispatcher, BlockGetHandler_l3, BlockPayload )
    {
        MKV_SWITCH_INIT();

        E_CASE( KaxBlock, kblock )
        {
            vars.block = &kblock;
            vars.block->ReadData( vars.obj->es.I_O() );
            vars.block->SetParent( *vars.obj->cluster );

            tracks_map_t::iterator track_it;
            bool const b_valid_track = !vars.obj->FindTrackByBlock( &track_it, &kblock, NULL );
            if( b_valid_track && track_it->second->fmt.i_cat == SPU_ES )
            {
                vars.obj->_seeker.add_seekpoint( kblock.TrackNum(), SegmentSeeker::Seekpoint::TRUSTED, kblock.GetElementPosition(), kblock.GlobalTimecode() / 1000 );
            }

            vars.obj->ep->Keep ();
        }
        E_CASE( KaxBlockDuration, kduration )
        {
            kduration.ReadData( vars.obj->es.I_O() );
            vars.i_duration = static_cast<uint64>( kduration );
        }
        E_CASE( KaxReferenceBlock, kreference )
        {
           kreference.ReadData( vars.obj->es.I_O() );

           if( vars.b_key_picture )
               vars.b_key_picture = false;
           else if( static_cast<int64>( kreference ) )
               vars.b_discardable_picture = true;
        }
        E_CASE( KaxClusterSilentTrackNumber, kstrackn )
        {
            VLC_UNUSED( kstrackn );
            VLC_UNUSED( vars );
        }
#if LIBMATROSKA_VERSION >= 0x010401
        E_CASE( KaxDiscardPadding, kdiscardp )
        {
            kdiscardp.ReadData( vars.obj->es.I_O() );
            int64 i_duration = static_cast<int64>( kdiscardp );

            if( vars.i_duration < i_duration )
                vars.i_duration = 0;
            else
                vars.i_duration -= i_duration;
        }
#endif
        E_CASE_DEFAULT( element )
        {
            VLC_UNUSED(element);

            msg_Warn( vars.p_demuxer, "unknown element at { fpos: %" PRId64 ", '%s' }",
              element.GetElementPosition(), typeid( element ).name() );
        }
    };

    static EbmlTypeDispatcher const * const dispatchers[] = {
        &BlockGetHandler_l1::Dispatcher(),
        &BlockGetHandler_l2::Dispatcher(),
        &BlockGetHandler_l3::Dispatcher()
    };

    for( ;; )
    {
        EbmlElement *el = NULL;
        int         i_level;

        if ( ep == NULL )
            return VLC_EGENERIC;

        if( pp_simpleblock != NULL || ((el = ep->Get()) == NULL && pp_block != NULL) )
        {
            /* Check blocks validity to protect againts broken files */
            if( FindTrackByBlock( &track_it, pp_block , pp_simpleblock ) )
            {
                ep->Unkeep();
                pp_simpleblock = NULL;
                pp_block = NULL;
                continue;
            }
            if( pp_simpleblock != NULL )
            {
                *pb_key_picture         = pp_simpleblock->IsKeyframe();
                *pb_discardable_picture = pp_simpleblock->IsDiscardable();
            }
            /* We have block group let's check if the picture is a keyframe */
            else if( *pb_key_picture )
            {
                if( track_it->second->fmt.i_codec == VLC_CODEC_THEORA )
                {
                    DataBuffer *    p_data = &pp_block->GetBuffer(0);
                    const uint8_t * p_buff = p_data->Buffer();
                    /* if the second bit of a Theora frame is 1
                       it's not a keyframe */
                    if( p_data->Size() && p_buff )
                    {
                        if( p_buff[0] & 0x40 )
                            *pb_key_picture = false;
                    }
                    else
                        *pb_key_picture = false;
                }
            }

            return VLC_SUCCESS;
        }

        i_level = ep->GetLevel();

        if( el == NULL )
        {
            if( i_level > 1 )
            {
                ep->Up();
                continue;
            }
            msg_Warn( &sys.demuxer, "EOF" );
            return VLC_EGENERIC;
        }

        /* Verify that we are still inside our cluster
         * It can happens whith broken files and when seeking
         * without index */
        if( i_level > 1 )
        {
            if( cluster && !ep->IsTopPresent( cluster ) )
            {
                msg_Warn( &sys.demuxer, "Unexpected escape from current cluster" );
                cluster = NULL;
            }
            if( !cluster )
                continue;
        }

        /* do parsing */

        try {
            switch( i_level )
            {
                case 2:
                case 3:
                    if( unlikely( !el->ValidateSize() || ( el->IsFiniteSize() && el->GetSize() >= SIZE_MAX ) ) )
                    {
                        msg_Err( &sys.demuxer, "Error while reading %s... upping level", typeid(*el).name());
                        ep->Up();

                        if ( i_level == 2 )
                            break;

                        ep->Unkeep();
                        pp_simpleblock = NULL;
                        pp_block = NULL;

                        break;
                    }
                case 1:
                    {
                        EbmlTypeDispatcher const * dispatcher = dispatchers[i_level - 1];
                        dispatcher->send( el, BlockGetHandler_l1::Payload( payload ) );
                    }
                    break;

                default:
                    msg_Err( &sys.demuxer, "invalid level = %d", i_level );
                    return VLC_EGENERIC;
            }
        }
        catch (int ret_code)
        {
            return ret_code;
        }
        catch (...)
        {
            msg_Err( &sys.demuxer, "Error while reading %s... upping level", typeid(*el).name());
            ep->Up();
            ep->Unkeep();
            pp_simpleblock = NULL;
            pp_block = NULL;
        }
    }
}
