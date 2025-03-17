/*************************************************************************************************
*
* File: MIDI.cpp
*
* Description: Implements the MIDI objects
*
* Copyright (c) 2010 Brian Pantano. All rights reserved.
*
*************************************************************************************************/
#include "MIDI.h"
#include <fstream>
#include <stack>
#include <array>
#include <ppl.h>
#include <intrin.h>
#include <smmintrin.h>
#include "lzma.h"

//std::map<int, std::pair<std::vector<MIDIEvent*>::iterator, std::vector<MIDIEvent*>>> midi_map;
MIDILoadingProgress g_LoadingProgress;

//-----------------------------------------------------------------------------
// MIDIPos functions
//-----------------------------------------------------------------------------

MIDIPos::MIDIPos( MIDI &midi ) : m_MIDI( midi )
{
    // Init file position
    m_iCurrTick = m_iCurrMicroSec = 0;

    // Init track positions
    size_t iTracks = m_MIDI.m_vTracks.size();
    size_t iTracksRounded = (iTracks + 8) & ~7; // Need to round up to 32 bytes, each int is 4 bytes
    m_pTrackTime = (int*)_aligned_malloc(iTracksRounded * sizeof(int), 32);
    for (size_t i = 0; i < iTracks; i++)
        m_vTrackPos.push_back(0);
    for (size_t i = 0; i < iTracksRounded; i++)
        m_pTrackTime[i] = INT_MAX;

    // Init SMPTE tempo
    if ( m_MIDI.m_Info.iDivision & 0x8000 )
    {
        int iFramesPerSec = -( ( m_MIDI.m_Info.iDivision | static_cast< int >( 0xFFFF0000 ) ) >> 8 ) * 100;
        if ( iFramesPerSec == 2900 ) iFramesPerSec = 2997;
        int iTicksPerFrame = m_MIDI.m_Info.iDivision & 0xFF;

        m_bIsStandard = false;
        m_iTicksPerBeat = m_iMicroSecsPerBeat = 0;
        m_iTicksPerSecond = iTicksPerFrame * iFramesPerSec;
    }
    // Init ticks per beat tempo (default to 120 BPM). x/y + 1/2 = (2x + y)/(2y)
    else
    {
        m_bIsStandard = true;
        m_iTicksPerSecond = 0;
        m_iTicksPerBeat = m_MIDI.m_Info.iDivision;
        m_iMicroSecsPerBeat = 500000;
    }
}

MIDIPos::~MIDIPos() {
    if (m_pTrackTime)
        _aligned_free(m_pTrackTime);
}

#ifdef __AVX2__
// https://github.com/WojciechMula/toys/blob/master/simd-min-index/avx2.cpp
size_t min_index_avx2(int32_t* array, size_t size) {
    const __m256i increment = _mm256_set1_epi32(8);
    __m256i indices = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
    __m256i minindices = indices;
    __m256i minvalues = _mm256_loadu_si256((__m256i*)array);

    for (size_t i = 8; i < size; i += 8) {

        indices = _mm256_add_epi32(indices, increment);

        const __m256i values = _mm256_loadu_si256((__m256i*)(array + i));
        const __m256i lt = _mm256_cmpgt_epi32(minvalues, values);
        minindices = _mm256_blendv_epi8(minindices, indices, lt);
        minvalues = _mm256_min_epi32(values, minvalues);
    }

    // find min index in vector result (in an extremely naive way)
    int32_t values_array[8];
    uint32_t indices_array[8];

    _mm256_storeu_si256((__m256i*)values_array, minvalues);
    _mm256_storeu_si256((__m256i*)indices_array, minindices);

    size_t  minindex = indices_array[0];
    int32_t minvalue = values_array[0];
    for (int i = 1; i < 8; i++) {
        if (values_array[i] < minvalue) {
            minvalue = values_array[i];
            minindex = indices_array[i];
        }
        else if (values_array[i] == minvalue) {
            minindex = min(minindex, size_t(indices_array[i]));
        }
    }

    return minindex;
}
#else
// https://github.com/WojciechMula/toys/blob/master/simd-min-index/sse.cpp
size_t min_index_sse(int32_t* array, size_t size) {
    const __m128i increment = _mm_set1_epi32(4);
    __m128i indices = _mm_setr_epi32(0, 1, 2, 3);
    __m128i minindices = indices;
    __m128i minvalues = _mm_loadu_si128((__m128i*)array);

    for (size_t i = 4; i < size; i += 4) {

        indices = _mm_add_epi32(indices, increment);

        const __m128i values = _mm_loadu_si128((__m128i*)(array + i));
        const __m128i lt = _mm_cmplt_epi32(values, minvalues);
        minindices = _mm_blendv_epi8(minindices, indices, lt);
        minvalues = _mm_min_epi32(values, minvalues);
    }

    // find min index in vector result (in an extremely naive way)
    int32_t values_array[4];
    uint32_t indices_array[4];

    _mm_storeu_si128((__m128i*)values_array, minvalues);
    _mm_storeu_si128((__m128i*)indices_array, minindices);

    size_t  minindex = indices_array[0];
    int32_t minvalue = values_array[0];
    for (int i = 1; i < 4; i++) {
        if (values_array[i] < minvalue) {
            minvalue = values_array[i];
            minindex = indices_array[i];
        }
        else if (values_array[i] == minvalue) {
            minindex = min(minindex, size_t(indices_array[i]));
        }
    }

    return minindex;
}
#endif

// Gets the next closest event as long as it occurs before iMicroSecs elapse
// Always get next event if iMicroSecs is negative
int MIDIPos::GetNextEvent( int iMicroSecs, MIDIEvent **pOutEvent )
{
    if ( !pOutEvent ) return 0;
    *pOutEvent = NULL;

    // Get the next closest event
    size_t iTracks = m_vTrackPos.size();
#ifdef __AVX2__
    int iMinPos = (int)min_index_avx2(m_pTrackTime, (iTracks + 8) & ~7);
#else
    int iMinPos = (int)min_index_sse(m_pTrackTime, (iTracks + 8) & ~7);
#endif
    if (m_pTrackTime[iMinPos] == INT_MAX)
        return 0;

    MIDIEvent* pMinEvent = m_MIDI.m_vTracks[iMinPos]->m_vEvents[m_vTrackPos[iMinPos]];

    // Make sure the event doesn't occur after the requested time window
    int iMaxTickAllowed = m_iCurrTick;
    if (m_bIsStandard) {
        if (m_iMicroSecsPerBeat != 0)
            iMaxTickAllowed += (static_cast<long long>(m_iTicksPerBeat) * (m_iCurrMicroSec + iMicroSecs)) / m_iMicroSecsPerBeat;
    } else {
        iMaxTickAllowed += (static_cast<long long>(m_iTicksPerSecond) * (m_iCurrMicroSec + iMicroSecs)) / 1000000;
    }

    if ( iMicroSecs < 0 || pMinEvent->GetAbsT() <= iMaxTickAllowed )
    {
        // How many micro seconds did we just process?
        *pOutEvent = pMinEvent;
        int iSpan = pMinEvent->GetAbsT() - m_iCurrTick;
        if ( m_bIsStandard )
            iSpan = ( static_cast< long long >( m_iMicroSecsPerBeat ) * iSpan ) / m_iTicksPerBeat - m_iCurrMicroSec;
        else
            iSpan = ( 1000000LL * iSpan ) / m_iTicksPerSecond - m_iCurrMicroSec;
        m_iCurrTick = pMinEvent->GetAbsT();
        m_iCurrMicroSec = 0;
        m_vTrackPos[iMinPos]++;
        m_pTrackTime[iMinPos] = m_vTrackPos[iMinPos] == m_MIDI.m_vTracks[iMinPos]->m_vEvents.size() ? INT_MAX : m_MIDI.m_vTracks[iMinPos]->m_vEvents[m_vTrackPos[iMinPos]]->GetAbsT();

        // Change the tempo going forward if we're at a SetTempo event
        if ( pMinEvent->GetEventType() == MIDIEvent::MetaEvent )
        {
            MIDIMetaEvent *pMetaEvent = reinterpret_cast< MIDIMetaEvent* >( pMinEvent );
            if ( pMetaEvent->GetMetaEventType() == MIDIMetaEvent::SetTempo && pMetaEvent->GetDataLen() == 3 )
                MIDI::Parse24Bit ( pMetaEvent->GetData(), 3, &m_iMicroSecsPerBeat );
        }

        return iSpan;
    }
    // No events to be found, but haven't hit end of file
    else
    {
        if ( m_bIsStandard )
            m_iCurrMicroSec = iMicroSecs + m_iCurrMicroSec -
                              ( static_cast< long long >( m_iMicroSecsPerBeat ) * ( iMaxTickAllowed - m_iCurrTick ) ) / m_iTicksPerBeat;
        else
            m_iCurrMicroSec = iMicroSecs + m_iCurrMicroSec -
                              ( 1000000LL * ( iMaxTickAllowed - m_iCurrTick ) ) / m_iTicksPerSecond;
        m_iCurrTick = iMaxTickAllowed;
        return iMicroSecs;
    }
}

int MIDIPos::GetNextEvents( int iMicroSecs, vector< MIDIEvent* > &vEvents )
{
    MIDIEvent *pEvent = NULL;
    int iTotal = 0;
    do
    {
        if ( iMicroSecs >= 0 )
            iTotal += GetNextEvent( iMicroSecs - iTotal, &pEvent );
        else
            iTotal += GetNextEvent( iMicroSecs, &pEvent );
        if ( pEvent ) vEvents.push_back( pEvent );
    }
    while ( pEvent );

    return iTotal;
}

//-----------------------------------------------------------------------------
// MIDI functions
//-----------------------------------------------------------------------------

MIDI::MIDI ( const wstring &sFilename )
{
    FILE* stream;

    // Open the file
    if (_wfopen_s(&stream, sFilename.c_str(), L"rb") == 0)
    {
        // Go to the end of the file to get the max size
        _fseeki64(stream, 0, SEEK_END);
        size_t iSize = static_cast<size_t>(_ftelli64(stream));
        unsigned char* pcMemBlock = new unsigned char[iSize];

        // Go to the beginning of the file to prepare for parsing
        if (_fseeki64(stream, 0, SEEK_SET)) {
            MessageBoxA(NULL, "_fseeki64 encountered an error.", "Piano From Above", MB_OK | MB_ICONERROR);
            return;
        }

        // Parse the entire MIDI to memory
        fread(reinterpret_cast<char*>(pcMemBlock), 1, iSize, stream);

        // Close the stream, since it's not needed anymore
        fclose(stream);

        // Decompress the MIDI if needed
        constexpr uint8_t lzma_magic[] = {0xFD, 0x37, 0x7A, 0x58, 0x5A, 0x00};
        while (iSize >= LZMA_STREAM_HEADER_SIZE * 2 && !memcmp(pcMemBlock, lzma_magic, sizeof(lzma_magic))) {
            auto compressed = pcMemBlock;

            // Get the decompressed size
            // This is a real pain in the ass for concatenated .xz files, lots of sanity checking is skipped here
            // See https://github.com/kobolabs/liblzma/blob/master/src/xz/list.c
            char err[1024] = {};
            uint64_t decompressed_size = 0;
            lzma_stream strm = LZMA_STREAM_INIT;
            lzma_stream_flags stream_flags;
            lzma_index* index = nullptr;
            auto pos = (int64_t)iSize;
            lzma_ret ret;
            do {
                // Position sanity check
                if (pos < LZMA_STREAM_HEADER_SIZE * 2) {
                    MessageBoxA(NULL, "Position sanity check failed. Corrupted file?", "Piano From Above", MB_OK | MB_ICONERROR);
                    lzma_index_end(index, NULL);
                    delete[] pcMemBlock;
                    return;
                }
                pos -= LZMA_STREAM_HEADER_SIZE;

                // Locate and decode stream footer
                uint64_t footer_pos;
                while (true) {
                    if (pos < LZMA_STREAM_HEADER_SIZE) {
                        MessageBoxA(NULL, "Locating stream footer failed. Corrupted file?", "Piano From Above", MB_OK | MB_ICONERROR);
                        lzma_index_end(index, NULL);
                        delete[] pcMemBlock;
                        return;
                    }
                    footer_pos = pos;

                    int i = 2;
                    if (*(uint32_t*)&compressed[footer_pos + 8] != 0)
                        break;

                    do {
                        pos -= 4;
                        --i;
                    } while (i >= 0 && *(uint32_t*)&compressed[footer_pos + i * 4] == 0);
                }
                ret = lzma_stream_footer_decode(&stream_flags, &compressed[footer_pos]);
                if (ret != LZMA_OK) {
                    snprintf(err, sizeof(err) - 1, "Decoding stream footer failed: %d\nCorrupt file?", ret);
                    MessageBoxA(NULL, err, "Piano From Above", MB_OK | MB_ICONERROR);
                    lzma_index_end(index, NULL);
                    delete[] pcMemBlock;
                    return;
                }
                if ((lzma_vli)pos < stream_flags.backward_size + LZMA_STREAM_HEADER_SIZE) {
                    MessageBoxA(NULL, "Stream footer position sanity check failed. Corrupted file?", "Piano From Above", MB_OK | MB_ICONERROR);
                    lzma_index_end(index, NULL);
                    delete[] pcMemBlock;
                    return;
                }

                // Decode index
                pos -= stream_flags.backward_size;
                lzma_index_decoder(&strm, &index, UINT64_MAX);
                strm.avail_in = stream_flags.backward_size;
                strm.next_in = &compressed[pos];
                pos += stream_flags.backward_size;
                ret = lzma_code(&strm, LZMA_RUN);
                if (ret != LZMA_STREAM_END) {
                    snprintf(err, sizeof(err) - 1, "Index decode failed: %d\nCorrupt file?", ret);
                    MessageBoxA(NULL, err, "Piano From Above", MB_OK | MB_ICONERROR);
                    lzma_index_end(index, NULL);
                    delete[] pcMemBlock;
                    return;
                }
                pos -= stream_flags.backward_size + LZMA_STREAM_HEADER_SIZE;
                if ((lzma_vli)pos < lzma_index_total_size(index)) {
                    MessageBoxA(NULL, "Index position sanity check failed. Corrupted file?", "Piano From Above", MB_OK | MB_ICONERROR);
                    lzma_index_end(index, NULL);
                    delete[] pcMemBlock;
                    return;
                }
                pos -= lzma_index_total_size(index);
                decompressed_size += lzma_index_uncompressed_size(index);
            } while (pos > 0);

            // Initialize progress
            g_LoadingProgress.stage = MIDILoadingProgress::Stage::Decompress;
            g_LoadingProgress.progress = 0;
            g_LoadingProgress.max = decompressed_size;
            
            // Decompress it
            pcMemBlock = new unsigned char[decompressed_size];
            uint8_t* write_ptr = pcMemBlock;
            lzma_end(&strm);
            strm = LZMA_STREAM_INIT;
            lzma_stream_decoder(&strm, UINT64_MAX, LZMA_CONCATENATED);
            strm.next_in = compressed;
            strm.avail_in = iSize;
            bool done = false;
            lzma_action action = LZMA_RUN;
            while (!done) {
                if (strm.avail_in == 0)
                    action = LZMA_FINISH;
                lzma_ret ret = lzma_code(&strm, action);
                if (strm.avail_out == 0) {
                    auto remaining = min(decompressed_size - (write_ptr - pcMemBlock), 1 << 20);
                    strm.next_out = write_ptr;
                    strm.avail_out = remaining;
                    g_LoadingProgress.progress = write_ptr - pcMemBlock;
                    write_ptr += remaining;
                }
                switch (ret) {
                case LZMA_STREAM_END:
                    done = true;
                    break;
                case LZMA_OK:
                    break;
                case LZMA_MEM_ERROR:
                    MessageBoxA(NULL, "Ran out of memory while decompressing.", "Piano From Above", MB_OK | MB_ICONERROR);
                    delete[] compressed;
                    delete[] pcMemBlock;
                    return;
                default:
                    snprintf(err, sizeof(err) - 1, "An error occurred while decompressing: %d\nCorrupt file?", ret);
                    MessageBoxA(NULL, err, "Piano From Above", MB_OK | MB_ICONERROR);
                    delete[] compressed;
                    delete[] pcMemBlock;
                    return;
                }
            }
            iSize = decompressed_size;
            delete[] compressed;
        }

        // Parse it
        ParseMIDI(pcMemBlock, iSize);
        m_Info.sFilename = sFilename;

        // Clean up
        delete[] pcMemBlock;
    }
}

MIDI::~MIDI( void )
{
    clear();
}

#define EVENT_POOL_MAX 1000000
MIDIChannelEvent* MIDI::AllocChannelEvent() {
    if (event_pools.size() == 0 || event_pools.back().count == EVENT_POOL_MAX) {
        // Currently, MIDIChannelEvent is 32 bytes large.
        // This is conveniently exactly half the size of an x86 cache line.
        // Making sure the pool allocation is aligned to at least 32 bytes should ensure that all member accesses are in cache.
        static_assert(sizeof(MIDIChannelEvent) == 32);
        event_pools.emplace_back();
        event_pools.back().events = (MIDIChannelEvent*)_aligned_malloc(EVENT_POOL_MAX * sizeof(MIDIChannelEvent), 32);
        event_pools.back().count = 0;
    }
    auto& pool = event_pools.back();
    auto ev = &pool.events[pool.count++];
    new (ev) MIDIChannelEvent();
    return ev;
}


const wstring MIDI::Instruments[129] =
{
    L"Acoustic Grand Piano", L"Bright Acoustic Piano", L"Electric Grand Piano", L"Honky-tonk Piano", L"Electric Piano 1", 
    L"Electric Piano 2", L"Harpsichord", L"Clavi", L"Celesta", L"Glockenspiel", 
    L"Music Box", L"Vibraphone", L"Marimba", L"Xylophone", L"Tubular Bells", 
    L"Dulcimer", L"Drawbar Organ", L"Percussive Organ", L"Rock Organ", L"Church Organ", 
    L"Reed Organ", L"Accordion", L"Harmonica", L"Tango Accordion", L"Acoustic Guitar (nylon)", 
    L"Acoustic Guitar (steel)", L"Electric Guitar (jazz)", L"Electric Guitar (clean)", L"Electric Guitar (muted)", L"Overdriven Guitar", 
    L"Distortion Guitar", L"Guitar harmonics", L"Acoustic Bass", L"Electric Bass (finger)", L"Electric Bass (pick)", 
    L"Fretless Bass", L"Slap Bass 1", L"Slap Bass 2", L"Synth Bass 1", L"Synth Bass 2", 
    L"Violin", L"Viola", L"Cello", L"Contrabass", L"Tremolo Strings", 
    L"Pizzicato Strings", L"Orchestral Harp", L"Timpani", L"String Ensemble 1", L"String Ensemble 2", 
    L"SynthStrings 1", L"SynthStrings 2", L"Choir Aahs", L"Voice Oohs", L"Synth Voice", 
    L"Orchestra Hit", L"Trumpet", L"Trombone", L"Tuba", L"Muted Trumpet", 
    L"French Horn", L"Brass Section", L"SynthBrass 1", L"SynthBrass 2", L"Soprano Sax", 
    L"Alto Sax", L"Tenor Sax", L"Baritone Sax", L"Oboe", L"English Horn", 
    L"Bassoon", L"Clarinet", L"Piccolo", L"Flute", L"Recorder", 
    L"Pan Flute", L"Blown Bottle", L"Shakuhachi", L"Whistle", L"Ocarina", 
    L"Lead 1 (square)", L"Lead 2 (sawtooth)", L"Lead 3 (calliope)", L"Lead 4 (chiff)", L"Lead 5 (charang)", 
    L"Lead 6 (voice)", L"Lead 7 (fifths)", L"Lead 8 (bass + lead)", L"Pad 1 (new age)", L"Pad 2 (warm)", 
    L"Pad 3 (polysynth)", L"Pad 4 (choir)", L"Pad 5 (bowed)", L"Pad 6 (metallic)", L"Pad 7 (halo)", 
    L"Pad 8 (sweep)", L"FX 1 (rain)", L"FX 2 (soundtrack)", L"FX 3 (crystal)", L"FX 4 (atmosphere)", 
    L"FX 5 (brightness)", L"FX 6 (goblins)", L"FX 7 (echoes)", L"FX 8 (sci-fi)", L"Sitar", 
    L"Banjo", L"Shamisen", L"Koto", L"Kalimba", L"Bag pipe", 
    L"Fiddle", L"Shanai", L"Tinkle Bell", L"Agogo", L"Steel Drums", 
    L"Woodblock", L"Taiko Drum", L"Melodic Tom", L"Synth Drum", L"Reverse Cymbal", 
    L"Guitar Fret Noise", L"Breath Noise", L"Seashore", L"Bird Tweet", L"Telephone Ring",
    L"Helicopter", L"Applause", L"Gunshot", L"Various"
};

const wstring &MIDI::NoteName( int iNote )
{
    InitArrays();
    if ( iNote < 0 || iNote >= MIDI::KEYS ) return aNoteNames[MIDI::KEYS];
    return aNoteNames[iNote];
}

MIDI::Note MIDI::NoteVal( int iNote )
{
    InitArrays();
    if ( iNote < 0 || iNote >= MIDI::KEYS ) return C;
    return aNoteVal[iNote];
}

bool MIDI::IsSharp( int iNote )
{
    /*
    InitArrays();
    if ( iNote < 0 || iNote >= MIDI::KEYS ) return false;
    return aIsSharp[iNote];
    */
    return (1 << (iNote % 12)) & 0b010101001010;
}

// Number of white keys in [iMinNote, iMaxNote)
int MIDI::WhiteCount( int iMinNote, int iMaxNote )
{
    InitArrays();
    if ( iMinNote < 0 || iMinNote >= MIDI::KEYS || iMaxNote < 0 || iMaxNote >= MIDI::KEYS ) return false;
    return aWhiteCount[iMaxNote] - aWhiteCount[iMinNote];
}

wstring MIDI::aNoteNames[MIDI::KEYS + 1];
MIDI::Note MIDI::aNoteVal[MIDI::KEYS];
bool MIDI::aIsSharp[MIDI::KEYS];
int MIDI::aWhiteCount[MIDI::KEYS + 1];

void MIDI::InitArrays()
{
    static bool bValid = false;

    // Build the array of note names upon first call
    if ( !bValid )
    {
        wchar_t buf[10];
        wchar_t cNote = L'C';
        int iOctave = -1;
        bool bIsSharp = false;
        MIDI::Note eNote = MIDI::C;
        for ( int i = 0; i < MIDI::KEYS; i++ )
        {
            // Don't want sprintf because we're in c++ and string building is too slow. Manual construction!
            int iPos = 0;
            buf[iPos++] = cNote;
            if ( bIsSharp ) buf[iPos++] = L'#';
            if ( iOctave < 0 ) buf[iPos++] = L'-';
            buf[iPos++] = L'0' + abs( iOctave );
            buf[iPos++] = L'\0';

            aNoteNames[i] = buf;
            aNoteVal[i] = eNote;
            aIsSharp[i] = bIsSharp;

            // Advance counters
            if ( eNote == MIDI::B || eNote == MIDI::E || bIsSharp )
                cNote++;
            if ( eNote != MIDI::B && eNote != MIDI::E )
                bIsSharp = !bIsSharp;
            if ( eNote == MIDI::B )
                iOctave++;
            if ( eNote == MIDI::GS )
            {
                cNote = 'A';
                eNote = MIDI::A;
            }
            else
                eNote = static_cast< MIDI::Note >( eNote + 1 );
        }
        aWhiteCount[0] = 0;
        for ( int i = 1; i < MIDI::KEYS + 1; i++ )
            aWhiteCount[i] = aWhiteCount[i - 1] + !aIsSharp[i - 1];
        aNoteNames[MIDI::KEYS] = L"Invalid";
        bValid = true;
    }
}

void MIDI::clear( void )
{
    for ( vector< MIDITrack* >::iterator it = m_vTracks.begin(); it != m_vTracks.end(); ++it )
        delete *it;
    m_vTracks.clear();
    m_Info.clear();
    for (auto& pool : event_pools)
        _aligned_free(pool.events);
    event_pools.clear();
}

size_t MIDI::ParseMIDI( const unsigned char *pcData, size_t iMaxSize )
{
    char pcBuf[4];
    size_t iTotal;
    uint32_t iHdrSize;

    // Reset first. This is the only parsing function that resets/clears first.
    clear();

    // Read header info
    if ( ParseNChars( pcData, 4, iMaxSize, pcBuf ) != 4 ) return 0;
    if ( Parse32Bit( pcData + 4, iMaxSize - 4, &iHdrSize) != 4 ) return 0;
    iTotal = 8;

    // Check header info
    if ( strncmp( pcBuf, "MThd", 4 ) != 0 ) return 0;
    iHdrSize = max( iHdrSize, 6 ); // Allowing a bad header size. Some people ignore and hard code 6.
    
    //Read header
    iTotal += Parse16Bit( pcData + iTotal, iMaxSize - iTotal, &m_Info.iFormatType );
    iTotal += Parse16Bit( pcData + iTotal, iMaxSize - iTotal, &m_Info.iNumTracks );
    iTotal += Parse16Bit( pcData + iTotal, iMaxSize - iTotal, &m_Info.iDivision );

    // Check header
    if ( iTotal != 14 || m_Info.iFormatType < 0 || m_Info.iFormatType > 2 || m_Info.iDivision == 0 ) return 0;

    // Parse the rest of the file
    iTotal += iHdrSize - 6;
    return iTotal + ParseTracks( pcData + iTotal, iMaxSize - iTotal );
}

size_t MIDI::ParseTracks( const unsigned char *pcData, size_t iMaxSize )
{
    size_t iTotal = 0, iCount = 0, iTrack = m_vTracks.size();
    g_LoadingProgress.stage = MIDILoadingProgress::Stage::ParseTracks;
    g_LoadingProgress.progress = 0;
    g_LoadingProgress.max = m_Info.iNumTracks; // not actually guaranteed to hit this
    do
    {
        // Create and parse the track
        MIDITrack *track = new MIDITrack(*this);
        iCount = track->ParseTrack( pcData + iTotal, iMaxSize - iTotal, iTrack++ );

        // If Success, add it to the list
        if ( iCount > 0 )
        {
            m_vTracks.push_back( track );
            m_Info.AddTrackInfo( *track );
            g_LoadingProgress.progress++;
        }
        else
            delete track;

        iTotal += iCount;
    }
    while ( iMaxSize - iTotal > 0 && iCount > 0 && m_Info.iFormatType != 2 );

    // Some MIDIs lie about the amount of tracks
    m_Info.iNumTracks = (int)m_vTracks.size();

    return iTotal;
}

size_t MIDI::ParseEvents( const unsigned char *pcData, size_t iMaxSize )
{
    // Create and parse the track
    MIDITrack *track = new MIDITrack(*this);
    size_t iCount = track->ParseEvents( pcData, iMaxSize, m_vTracks.size());

    // If Success, add it to the list
    if ( iCount > 0 ) {
        m_vTracks.push_back( track );
        m_Info.AddTrackInfo( *track );
    }
    else
        delete track;

    return iCount;
}

// Computes some of the MIDIInfo info
void MIDI::MIDIInfo::AddTrackInfo( const MIDITrack &mTrack )
{
    const MIDITrack::MIDITrackInfo &mti = mTrack.GetInfo();
    this->iTotalTicks = max( this->iTotalTicks, mti.iTotalTicks );
    this->iEventCount += mti.iEventCount;
    this->iNumChannels += mti.iNumChannels;
    this->iVolumeSum += mti.iVolumeSum;
    if ( mti.iNoteCount )
    {
        if ( !this->iNoteCount )
        {
            this->iMinNote = mti.iMinNote;
            this->iMaxNote = mti.iMaxNote;
            this->iMaxVolume = mti.iMaxVolume;
        }
        else
        {
            this->iMinNote = min( mti.iMinNote, this->iMinNote );
            this->iMaxNote = max( mti.iMaxNote, this->iMaxNote );
            this->iMaxVolume = max( mti.iMaxVolume, this->iMaxVolume );
        }
    }
    this->iNoteCount += mti.iNoteCount;
    if ( !( this->iDivision & 0x8000 ) && this->iDivision > 0 )
        this->iTotalBeats = this->iTotalTicks / this->iDivision;
}

// Sets absolute time variables. A lot of code for not much happening...
// Has to be EXACT. Even a little drift and things start messing up a few minutes in (metronome, etc)
void MIDI::PostProcess(vector<MIDIChannelEvent*>& vChannelEvents, eventvec_t* vProgramChanges, vector<MIDIMetaEvent*>* vMetaEvents, eventvec_t* vTempo, eventvec_t* vSignature, eventvec_t* vMarkers)
{
    // Iterator like class
    MIDIPos midiPos( *this );
    bool bIsStandard = midiPos.IsStandard();
    int iTicksPerBeat = midiPos.GetTicksPerBeat();
    int iTicksPerSecond = midiPos.GetTicksPerSecond();
    int iMicroSecsPerBeat = midiPos.GetMicroSecsPerBeat();
    int iLastTempoTick = 0;
    long long llLastTempoTime = 0;
    int iSimultaneous = 0;

    size_t event_count = 0;
    for (size_t i = 0; i < m_vTracks.size(); i++) {
        event_count += m_vTracks[i]->m_vEvents.size();
        if (!m_vTracks[i]->m_vEvents.empty())
            midiPos.m_pTrackTime[i] = m_vTracks[i]->m_vEvents[0]->GetAbsT();
    }

    g_LoadingProgress.stage = MIDILoadingProgress::Stage::SortEvents;
    g_LoadingProgress.progress = 0;
    g_LoadingProgress.max = event_count;

    MIDIEvent *pEvent = NULL;
    long long llFirstNote = -1;
    long long llTime = 0;
    for ( midiPos.GetNextEvent( -1, &pEvent ); pEvent; midiPos.GetNextEvent( -1, &pEvent ) )
    {
        // Compute the exact time (off by at most a micro second... I don't feel like rounding)
        int iTick = pEvent->GetAbsT();
        if ( bIsStandard )
            llTime = llLastTempoTime + ( static_cast< long long >( iMicroSecsPerBeat ) * ( iTick - iLastTempoTick ) ) / iTicksPerBeat;
        else
            llTime = llLastTempoTime + ( 1000000LL * ( iTick - iLastTempoTick ) ) / iTicksPerSecond;
        pEvent->SetAbsMicroSec( llTime );

        if ( pEvent->GetEventType() == MIDIEvent::ChannelEvent )
        {
            MIDIChannelEvent *pChannelEvent = reinterpret_cast< MIDIChannelEvent* >( pEvent );
            pChannelEvent->SetSimultaneous(iSimultaneous);
            if ( pChannelEvent->HasSister() )
            {
                if ( pChannelEvent->GetChannelEventType() == MIDIChannelEvent::NoteOn &&
                     pChannelEvent->GetParam2() > 0 )
                {
                    if ( llFirstNote < 0  )
                        llFirstNote = llTime;
                    iSimultaneous++;
                }
                else
                    iSimultaneous--;
                auto sister = pChannelEvent->GetPassDone() ? pChannelEvent->GetSister(vChannelEvents) : pChannelEvent->GetSister(m_vTracks[pEvent->GetTrack()]->m_vEvents);
                sister->SetSisterIdx(vChannelEvents.size());
                sister->SetPassDone(true);
                if (pChannelEvent->GetChannelEventType() != MIDIChannelEvent::NoteOn ||
                    pChannelEvent->GetParam2() == 0) {
                    sister->SetLength(llTime - sister->GetAbsMicroSec());
                }
            }
            vChannelEvents.push_back(pChannelEvent);

            MIDIChannelEvent::ChannelEventType eEventType = pChannelEvent->GetChannelEventType();
            if (vProgramChanges && (eEventType == MIDIChannelEvent::ProgramChange || eEventType == MIDIChannelEvent::Controller || eEventType == MIDIChannelEvent::PitchBend))
                vProgramChanges->push_back(pair< long long, int >(pEvent->GetAbsMicroSec(), vChannelEvents.size() - 1));
        }
        else if ( pEvent->GetEventType() == MIDIEvent::MetaEvent )
        {
            MIDIMetaEvent *pMetaEvent = reinterpret_cast< MIDIMetaEvent* >( pEvent );
            if ( pMetaEvent->GetMetaEventType() == MIDIMetaEvent::SetTempo )
            {
                iTicksPerBeat = midiPos.GetTicksPerBeat();
                iTicksPerSecond = midiPos.GetTicksPerSecond();
                iMicroSecsPerBeat = midiPos.GetMicroSecsPerBeat();
                iLastTempoTick = iTick;
                llLastTempoTime = llTime;
            }

            if (vMetaEvents) {
                MIDIMetaEvent::MetaEventType eEventType = pMetaEvent->GetMetaEventType();
                vMetaEvents->push_back(pMetaEvent);
                switch (eEventType) {
                case MIDIMetaEvent::SetTempo:
                    if (vTempo)
                        vTempo->push_back(pair< long long, int >(pEvent->GetAbsMicroSec(), vMetaEvents->size() - 1));
                    break;
                case MIDIMetaEvent::TimeSignature:
                    if (vSignature)
                        vSignature->push_back(pair< long long, int >(pEvent->GetAbsMicroSec(), vMetaEvents->size() - 1));
                    break;
                case MIDIMetaEvent::Marker:
                    if (vMarkers)
                        vMarkers->push_back(pair< long long, int >(pEvent->GetAbsMicroSec(), vMetaEvents->size() - 1));
                    break;
                default:
                    break;
                }
            }
        }

        g_LoadingProgress.progress++;
    }

    // We don't need the track vectors anymore (saves 8 bytes per event!)
    for (auto track : m_vTracks)
        track->ClearEvents();

    m_Info.llTotalMicroSecs = llTime;
    m_Info.llFirstNote = max( 0LL, llFirstNote );
}

void MIDI::ConnectNotes()
{
    std::vector<std::array<std::stack<std::tuple<size_t, MIDIChannelEvent*>>, 128>> vStacks;
    vStacks.resize(m_vTracks.size() * 16);

    g_LoadingProgress.stage = MIDILoadingProgress::Stage::ConnectNotes;
    g_LoadingProgress.progress = 0;
    g_LoadingProgress.max = m_vTracks.size();

    concurrency::parallel_for(size_t(0), m_vTracks.size(), [&](int track) {
        vector< MIDIEvent* >& vEvents = m_vTracks[track]->m_vEvents;
        for (size_t i = 0; i < vEvents.size(); i++) {
            if (vEvents[i]->GetEventType() == MIDIEvent::ChannelEvent)
            {
                MIDIChannelEvent* pEvent = reinterpret_cast<MIDIChannelEvent*>(vEvents[i]);
                MIDIChannelEvent::ChannelEventType eEventType = pEvent->GetChannelEventType();
                int iChannel = pEvent->GetChannel();
                int iNote = pEvent->GetParam1();
                int iVelocity = pEvent->GetParam2();
                auto& sStack = vStacks[track * 16 + iChannel][iNote];

                if (eEventType == MIDIChannelEvent::NoteOn && iVelocity > 0) {
                    sStack.push(std::make_tuple(i, pEvent));
                }
                else if (eEventType == MIDIChannelEvent::NoteOff || eEventType == MIDIChannelEvent::NoteOn) {
                    if (!sStack.empty()) {
                        auto pTop = sStack.top();
                        sStack.pop();
                        pEvent->SetSisterIdx(std::get<0>(pTop));
                        std::get<1>(pTop)->SetSisterIdx(i);
                    }
                }
            }
        }
        g_LoadingProgress.progress++;
    });
}


//-----------------------------------------------------------------------------
// MIDITrack functions
//-----------------------------------------------------------------------------

MIDITrack::MIDITrack(MIDI& midi) : m_MIDI(midi) {};

MIDITrack::~MIDITrack( void )
{
    clear();
}

void MIDITrack::clear( void )
{
    // TODO: this is fucking awful
    for (auto it = m_vEvents.begin(); it != m_vEvents.end(); ++it) {
        if ((*it)->GetEventType() != MIDIEvent::EventType::ChannelEvent)
            delete* it;
    }
    m_vEvents.clear();
    m_TrackInfo.clear();
}

size_t MIDITrack::ParseTrack( const unsigned char *pcData, size_t iMaxSize, size_t iTrack )
{
    char pcBuf[4];
    size_t iTotal;
    uint32_t iTrkSize;

    // Reset first
    clear();

    // Read header
    if ( MIDI::ParseNChars( pcData, 4, iMaxSize, pcBuf ) != 4 )
        return 0;
    if ( MIDI::Parse32Bit( pcData + 4, iMaxSize - 4, &iTrkSize) != 4 )
        return 0;
    iTotal = 8;

    // Check header
    if ( strncmp( pcBuf, "MTrk", 4 ) != 0 )
        return 0;

    //return iTotal + ParseEvents( pcData + iTotal, iMaxSize - iTotal, iTrack );
    ParseEvents(pcData + iTotal, iMaxSize - iTotal, iTrack);
    return iTotal + iTrkSize;
}

size_t MIDITrack::ParseEvents( const unsigned char *pcData, size_t iMaxSize, size_t iTrack )
{
    int iDTCode = 0;
    size_t iTotal = 0, iCount = 0;
    MIDIEvent *pEvent = NULL;
    m_TrackInfo.iSequenceNumber = iTrack;

    do
    {
        // Create and parse the event
        iCount = 0;
        iDTCode = MIDIEvent::MakeNextEvent( m_MIDI, pcData + iTotal, iMaxSize - iTotal, iTrack, &pEvent );
        if ( iDTCode > 0 )
        {
            switch (pEvent->GetEventType())
            {
            case MIDIEvent::ChannelEvent: iCount = ((MIDIChannelEvent*)pEvent)->ParseEvent(pcData + iDTCode + iTotal, iMaxSize - iDTCode - iTotal); break;
            case MIDIEvent::MetaEvent: iCount = ((MIDIMetaEvent*)pEvent)->ParseEvent(pcData + iDTCode + iTotal, iMaxSize - iDTCode - iTotal); break;
            case MIDIEvent::SysExEvent: iCount = ((MIDISysExEvent*)pEvent)->ParseEvent(pcData + iDTCode + iTotal, iMaxSize - iDTCode - iTotal); break;
            default: break;
            }
            if ( iCount > 0 )
            {
                iTotal += iDTCode + iCount;
                m_vEvents.push_back( pEvent );
                m_TrackInfo.AddEventInfo( *pEvent );
            }
            else
                delete pEvent;
        }
    }
    // Until we've parsed all the data, the last parse failed, or the event signals the end of track
    while ( iMaxSize - iTotal > 0 && iCount > 0 &&
            ( pEvent->GetEventType() != MIDIEvent::MetaEvent ||
              reinterpret_cast< MIDIMetaEvent* >( pEvent )->GetMetaEventType() != MIDIMetaEvent::EndOfTrack ) );

    return iTotal;
}

// Computes some of the TrackInfo info
// DOES NOT DO: llTotalMicroSecs (because info's not available yet), iSequenceNumber default value (done in parse event)
void MIDITrack::MIDITrackInfo::AddEventInfo( const MIDIEvent &mEvent )
{
    //EventCount and TotalTicks
    this->iEventCount++;
    this->iTotalTicks = max( this->iTotalTicks, mEvent.GetAbsT() );

    switch ( mEvent.GetEventType() )
    {
        case MIDIEvent::MetaEvent:
        {
            const MIDIMetaEvent &mMetaEvent = reinterpret_cast< const MIDIMetaEvent & >( mEvent );
            MIDIMetaEvent::MetaEventType eMetaEventType = mMetaEvent.GetMetaEventType();
            switch ( eMetaEventType )
            {
                //SequenceName
                case MIDIMetaEvent::SequenceName:
                    this->sSequenceName.assign( reinterpret_cast< char* >( mMetaEvent.GetData() ), mMetaEvent.GetDataLen() );
                    break;
                //SequenceNumber
                case MIDIMetaEvent::SequenceNumber:
                    if ( mMetaEvent.GetDataLen() == 2)
                        MIDI::Parse16Bit( mMetaEvent.GetData(), 2, &this->iSequenceNumber );
                    break;
                default:
                    break;
            }
            break;
        }
        case MIDIEvent::ChannelEvent:
        {
            const MIDIChannelEvent &mChannelEvent = reinterpret_cast< const MIDIChannelEvent & >( mEvent );
            MIDIChannelEvent::ChannelEventType eChannelEventType = mChannelEvent.GetChannelEventType();
            int iChannel = mChannelEvent.GetChannel();
            int iParam1 = mChannelEvent.GetParam1();
            int iParam2 = mChannelEvent.GetParam2();

            switch ( eChannelEventType )
            {
                case MIDIChannelEvent::NoteOn:
                    if ( iParam2 > 0 )
                    {
                        //MinNote and MaxNote
                        if ( !this->iNoteCount )
                        {
                            this->iMinNote = this->iMaxNote = iParam1;
                            this->iMaxVolume = iParam2;
                        }
                        else
                        {
                            this->iMinNote = min( iParam1, this->iMinNote );
                            this->iMaxNote = max( iParam1, this->iMaxNote );
                            this->iMaxVolume = max( iParam2, this->iMaxVolume );
                        }
                        //NoteCount
                        this->iNoteCount++;
                        this->iVolumeSum += iParam2;

                        //Channel info
                        if ( !this->aNoteCount[ iChannel ] )
                            this->iNumChannels++;
                        this->aNoteCount[ iChannel ]++;
                    }
                    break;
                // Should we break it down further?
                case MIDIChannelEvent::ProgramChange:
                    if ( this->aProgram[ iChannel ] != iParam1 )
                    {
                        if ( this->aNoteCount[ iChannel ] > 0 )
                            this->aProgram[ iChannel ] = 128; // Various
                        else
                            this->aProgram[ iChannel ] = iParam1;
                    }
                    break;
                default:
                    break;
            }
            break;
        }
        default:
            break;
    }
}

//-----------------------------------------------------------------------------
// MIDIEvent functions
//-----------------------------------------------------------------------------

MIDIEvent::EventType MIDIEvent::DecodeEventType( int iEventCode )
{
    if ( iEventCode < 0x80 ) return RunningStatus;
    if ( iEventCode < 0xF0 ) return ChannelEvent;
    if ( iEventCode < 0xFF ) return SysExEvent;
    return MetaEvent;
}

int MIDIEvent::MakeNextEvent( MIDI& midi, const unsigned char *pcData, size_t iMaxSize, int iTrack, MIDIEvent **pOutEvent )
{
    MIDIEvent *pPrevEvent = *pOutEvent;

    // Parse and check DT
    uint32_t iDT;
    uint32_t iTotal = MIDI::ParseVarNum ( pcData, iMaxSize, &iDT );
    if (iTotal == 0 || iMaxSize - iTotal < 1 ) return 0;

    // Parse and decode event code
    int iEventCode = pcData[iTotal];
    EventType eEventType = DecodeEventType( iEventCode );
    iTotal++;

    // Use previous event code for running status
    if ( eEventType == RunningStatus && pPrevEvent)
    {
        iEventCode = pPrevEvent->GetEventCode();
        eEventType = DecodeEventType( iEventCode );
        iTotal--;
    }

    // Make the object
    switch ( eEventType )
    {
        case ChannelEvent: *pOutEvent = midi.AllocChannelEvent(); break;
        case MetaEvent: *pOutEvent = new MIDIMetaEvent(); break;
        case SysExEvent: *pOutEvent = new MIDISysExEvent(); break;
        default: break;
    }

    (*pOutEvent)->m_eEventType = eEventType;
    (*pOutEvent)->m_iEventCode = iEventCode;
    (*pOutEvent)->m_iTrack = iTrack;
    (*pOutEvent)->m_iAbsT = iDT;
    if ( pPrevEvent ) (*pOutEvent)->m_iAbsT += pPrevEvent->m_iAbsT;

    /*
    auto& key = midi.midi_map[(*pOutEvent)->m_iAbsT];
    key.second.push_back(*pOutEvent);
    key.first = key.second.begin();
    if (key.second.size() == 1)
        midi.midi_map_times.push_back((*pOutEvent)->m_iAbsT);
    */

    return iTotal;
}

int MIDIChannelEvent::ParseEvent( const unsigned char *pcData, size_t iMaxSize )
{
    // Split up the event code
    m_cChannel = m_iEventCode & 0xF;

    // Parse one parameter
    if (static_cast<ChannelEventType>(m_iEventCode >> 4) == ProgramChange || static_cast<ChannelEventType>(m_iEventCode >> 4) == ChannelAftertouch)
    {
        if ( iMaxSize < 1 ) return 0;
        m_cParam1 = pcData[0];
        m_cParam2 = 0;
        return 1;
    }
    // Parse two parameters
    else
    {
        if ( iMaxSize < 2 ) return 0;
        m_cParam1 = pcData[0];
        m_cParam2 = pcData[1];
        return 2;
    }
}

int MIDIMetaEvent::ParseEvent( const unsigned char *pcData, size_t iMaxSize )
{
    if ( iMaxSize < 1 ) return 0;

    // Parse the code and the length
    m_eMetaEventType = static_cast< MetaEventType >( pcData[0] );
    uint32_t iCount = MIDI::ParseVarNum( pcData + 1, iMaxSize - 1, &m_iDataLen );
    if ( iCount == 0 || iMaxSize < 1 + iCount + m_iDataLen ) return 0;

    // Get the data
    if ( m_iDataLen > 0 )
    {
        m_pcData = new unsigned char[m_iDataLen];
        memcpy( m_pcData, pcData + 1 + iCount, m_iDataLen );
    }

    return 1 + iCount + m_iDataLen;
}

// NOTE: this is INCOMPLETE. Data is parsed but not fully interpreted:
// divided messages don't know about each other
int MIDISysExEvent::ParseEvent( const unsigned char *pcData, size_t iMaxSize )
{
    if ( iMaxSize < 1 ) return 0;

    // Parse the code and the length
    uint32_t iCount = MIDI::ParseVarNum( pcData, iMaxSize, &m_iDataLen );
    if ( iCount == 0 || iMaxSize < iCount + m_iDataLen ) return 0;

    // Get the data
    if ( m_iDataLen > 0 )
    {
        m_pcData = new unsigned char[m_iDataLen];
        memcpy( m_pcData, pcData + iCount, m_iDataLen );
        if ( m_iEventCode == 0xF0 && m_pcData[ m_iDataLen - 1 ] != 0xF7 )
            m_bHasMoreData = true;
    }

    return iCount + m_iDataLen;
}


//-----------------------------------------------------------------------------
// Parsing helpers
//-----------------------------------------------------------------------------

//Parse a variable length number from MIDI data
uint32_t MIDI::ParseVarNum( const unsigned char *pcData, size_t iMaxSize, uint32_t *piOut )
{
    if ( !pcData || !piOut || iMaxSize <= 0 )
        return 0;

    *piOut = 0;
    uint32_t i = 0;
    do
    {
        *piOut = ( *piOut << 7 ) | ( pcData[i] & 0x7F );
        i++;
    }
    while ( i < 4 && i < iMaxSize && ( pcData[i - 1] & 0x80 ) );

    return i;
}

//Parse 32 bits of data. Big Endian.
uint32_t MIDI::Parse32Bit( const unsigned char *pcData, size_t iMaxSize, uint32_t *piOut )
{
    if ( !pcData || !piOut || iMaxSize < 4 )
        return 0;

    *piOut = pcData[0];
    *piOut = ( *piOut << 8 ) | pcData[1];
    *piOut = ( *piOut << 8 ) | pcData[2];
    *piOut = ( *piOut << 8 ) | pcData[3];

    return 4;
}

//Parse 24 bits of data. Big Endian.
uint32_t MIDI::Parse24Bit( const unsigned char *pcData, size_t iMaxSize, uint32_t *piOut )
{
    if ( !pcData || !piOut || iMaxSize < 3 )
        return 0;

    *piOut = pcData[0];
    *piOut = ( *piOut << 8 ) | pcData[1];
    *piOut = ( *piOut << 8 ) | pcData[2];

    return 3;
}

//Parse 16 bits of data. Big Endian.
uint32_t MIDI::Parse16Bit( const unsigned char *pcData, size_t iMaxSize, uint32_t *piOut )
{
    if ( !pcData || !piOut || iMaxSize < 2 )
        return 0;

    *piOut = pcData[0];
    *piOut = ( *piOut << 8 ) | pcData[1];

    return 2;
}

//Parse a bunch of characters
uint32_t MIDI::ParseNChars( const unsigned char *pcData, size_t iNChars, size_t iMaxSize, char *pcOut )
{
    if ( !pcData || !pcOut || iMaxSize <= 0 )
        return 0;

    size_t iSize = min( iNChars, iMaxSize );
    memcpy( pcOut, pcData, iSize );

    return iSize;
}

//-----------------------------------------------------------------------------
// Device classes
//-----------------------------------------------------------------------------

// Port management functions
int MIDIOutDevice::GetNumDevs() const
{
    return midiOutGetNumDevs();
}

wstring MIDIOutDevice::GetDevName( int iDev ) const
{
    MIDIOUTCAPS moc;
    if ( midiOutGetDevCaps( iDev, &moc, sizeof( MIDIOUTCAPS ) ) == MMSYSERR_NOERROR )
        return moc.szPname;
    return wstring();
}

bool MIDIOutDevice::Open( int iDev )
{
    if ( m_bIsOpen ) Close();
    m_sDevice = GetDevName( iDev );
    m_bIsKDMAPI = false;

    MMRESULT mmResult = midiOutOpen( &m_hMIDIOut, iDev, ( DWORD_PTR )MIDIOutProc, ( DWORD_PTR )this, CALLBACK_FUNCTION );
    m_bIsOpen = ( mmResult == MMSYSERR_NOERROR );
    return m_bIsOpen;
}

bool MIDIOutDevice::OpenKDMAPI() {
    if (m_bIsOpen)
        Close();

    m_sDevice = L"KDMAPI";
    m_bIsKDMAPI = true;

    auto InitializeKDMAPIStream = (int(WINAPI*)())GetOmniMIDIProc("InitializeKDMAPIStream");
    *(FARPROC*)&SendDirectData = GetOmniMIDIProc("SendDirectData");
    *(FARPROC*)&GetVoiceCount = GetOmniMIDIProc("GetVoiceCount");
    *(FARPROC*)&GetRenderingTime = GetOmniMIDIProc("GetRenderingTime");

    m_bIsOpen = (SendDirectData && InitializeKDMAPIStream && InitializeKDMAPIStream());

    if (!m_bIsOpen)
        MessageBox(NULL, L"An error has occurred while loading KDMAPI.", L"ERROR", MB_OK | MB_SYSTEMMODAL | MB_ICONERROR);

    return m_bIsOpen;
}

uint64_t MIDIOutDevice::KDMAPIActiveVoices() {
    if (!m_bIsKDMAPI || !GetVoiceCount)
        return 0;

    return GetVoiceCount();
}

float MIDIOutDevice::KDMAPIRenderingTime() {
    if (!m_bIsKDMAPI || !GetRenderingTime)
        return 0;

    return GetRenderingTime();
}

void MIDIOutDevice::Close()
{
    if ( !m_bIsOpen ) return;

    if (m_bIsKDMAPI) {
        auto TerminateKDMAPIStream = (int(WINAPI*)())GetOmniMIDIProc("TerminateKDMAPIStream");
        if (TerminateKDMAPIStream)
            TerminateKDMAPIStream();
    } else {
        midiOutReset(m_hMIDIOut);
        midiOutClose(m_hMIDIOut);
    }
    m_bIsOpen = false;
}

void MIDIOutDevice::Reset()
{
    if (!m_bIsOpen) return;

    if (m_bIsKDMAPI) {
        auto ResetKDMAPIStream = (void(WINAPI*)())GetOmniMIDIProc("ResetKDMAPIStream");
        if (ResetKDMAPIStream)
            ResetKDMAPIStream();
    } else {
        midiOutReset(m_hMIDIOut);
    }
}

// Specialized midi functions
void MIDIOutDevice::AllNotesOff()
{
    PlayEventAcrossChannels( 0xB0, 0x7B, 0x00 ); // All notes off
    PlayEventAcrossChannels( 0xB0, 0x40, 0x00 ); // Sustain off
}

void MIDIOutDevice::AllNotesOff( const vector< int > &vChannels )
{
    PlayEventAcrossChannels( 0xB0, 0x7B, 0x00, vChannels );
    PlayEventAcrossChannels( 0xB0, 0x40, 0x00, vChannels );
}

void MIDIOutDevice::SetVolume( double dVolume )
{
    // No-op on KDMAPI
    if (!m_bIsKDMAPI) {
        DWORD dwVolume = static_cast<DWORD>(0xFFFF * dVolume + 0.5);
        midiOutSetVolume(m_hMIDIOut, dwVolume | (dwVolume << 16));
    }
}

// Play events
bool MIDIOutDevice::PlayEventAcrossChannels( unsigned char cStatus, unsigned char cParam1, unsigned char cParam2 )
{
    if ( !m_bIsOpen ) return false;

    cStatus &= 0xF0;
    bool bResult = true;
    for ( int i = 0; i < 16; i++ )
        bResult &= PlayEvent( cStatus + i, cParam1, cParam2 );

    return bResult;
}

bool MIDIOutDevice::PlayEventAcrossChannels( unsigned char cStatus, unsigned char cParam1, unsigned char cParam2, const vector< int > &vChannels )
{
    if ( !m_bIsOpen ) return false;

    cStatus &= 0xF0;
    bool bResult = true;
    for ( vector< int >::const_iterator it = vChannels.begin(); it != vChannels.end(); ++it )
        bResult &= PlayEvent( cStatus + *it, cParam1, cParam2 );

    return bResult;
}

bool MIDIOutDevice::PlayEvent( unsigned char cStatus, unsigned char cParam1, unsigned char cParam2 )
{
    if ( !m_bIsOpen ) return false;
    if (m_bIsKDMAPI) {
        SendDirectData((cParam2 << 16) + (cParam1 << 8) + cStatus);
        return true;
    } else {
        return midiOutShortMsg(m_hMIDIOut, (cParam2 << 16) + (cParam1 << 8) + cStatus) == MMSYSERR_NOERROR;
    }
}

FARPROC MIDIOutDevice::GetOmniMIDIProc(const char* func) {
    auto dll = GetModuleHandle(L"OmniMIDI");
    if (!dll)
        dll = LoadLibrary(L"OmniMIDI");
    return GetProcAddress(dll, func);
}

void CALLBACK MIDIOutDevice::MIDIOutProc(HMIDIOUT, UINT wMsg, DWORD_PTR, DWORD_PTR, DWORD_PTR)
{
    switch ( wMsg )
    {
        case MOM_CLOSE:
        {
        }
    }
}