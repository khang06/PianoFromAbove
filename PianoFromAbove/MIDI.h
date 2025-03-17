/*************************************************************************************************
*
* File: MIDI.h
*
* Description: Defines the MIDI objects
*
* Copyright (c) 2010 Brian Pantano. All rights reserved.
*
*************************************************************************************************/
#pragma once

#include <Windows.h>
#include <vector>
#include <string>
#include <map>
#include <atomic>
#include <stdint.h>
using namespace std;

#include "Misc.h"

//Classes defined in this file
class MIDI;
class MIDITrack;
class MIDIEvent;
class MIDIChannelEvent;
class MIDIMetaEvent;
class MIDISysExEvent;
class MIDIPos;

class MIDIOutDevice;

//
// MIDI File Classes
//

class MIDIPos
{
public:
    MIDIPos( MIDI &midi );
    ~MIDIPos();

    int GetNextEvent( int iMicroSecs, MIDIEvent **pEvent );
    int GetNextEvents( int iMicroSecs, vector< MIDIEvent* > &vEvents );

    bool IsStandard() const { return m_bIsStandard; }
    int GetTicksPerBeat() const { return m_iTicksPerBeat; }
    int GetTicksPerSecond() const { return m_iTicksPerSecond; }
    int GetMicroSecsPerBeat() const { return m_iMicroSecsPerBeat; }

    int* m_pTrackTime;

private:
    // Where are we in the file?
    MIDI &m_MIDI;
    vector< size_t > m_vTrackPos;

    // Tempo variables
    bool m_bIsStandard;
    uint32_t m_iTicksPerBeat, m_iMicroSecsPerBeat; // For standard division
    int m_iTicksPerSecond; // For SMPTE division

    // Position variables
    int m_iCurrTick;
    int m_iCurrMicroSec;
};

typedef vector< pair< long long, int > > eventvec_t;

//Holds MIDI data
class MIDI
{
public:
    enum Note { A, AS, B, C, CS, D, DS, E, F, FS, G, GS };

    static const int KEYS = 129; // One extra because 128th is a sharp
    static const int C8 = 108;
    static const int C4 = C8 - 4 * 12;
    static const int A0 = C8 - 7 * 12 - 3;
    static const int Drums = 0x09;
    static const wstring Instruments[129];
    static const wstring &NoteName( int iNote );
    static Note NoteVal( int iNote );
    static bool IsSharp( int iNote );
    static int WhiteCount( int iMinNote, int iMaxNote );

    //Generally usefull static parsing functions
    static uint32_t ParseVarNum( const unsigned char *pcData, size_t iMaxSize, uint32_t *piOut );
    static uint32_t Parse32Bit( const unsigned char *pcData, size_t iMaxSize, uint32_t *piOut );
    static uint32_t Parse24Bit( const unsigned char *pcData, size_t iMaxSize, uint32_t *piOut );
    static uint32_t Parse16Bit( const unsigned char *pcData, size_t iMaxSize, uint32_t *piOut );
    static uint32_t ParseNChars( const unsigned char *pcData, size_t iNChars, size_t iMaxSize, char *pcOut );

    MIDI( void ) {};
    MIDI( const wstring &sFilename );
    ~MIDI( void );

    // shitty memory pool allocator
    MIDIChannelEvent* AllocChannelEvent();

    //Parsing functions that load data into the instance
    size_t ParseMIDI( const unsigned char *pcData, size_t iMaxSize );
    size_t ParseTracks( const unsigned char *pcData, size_t iMaxSize );
    size_t ParseEvents( const unsigned char *pcData, size_t iMaxSize );
    bool IsValid() const { return ( m_vTracks.size() > 0 && m_Info.iNoteCount > 0 && m_Info.iDivision > 0 ); }

    void PostProcess(vector<MIDIChannelEvent*>& vChannelEvents, eventvec_t* vProgramChanges = nullptr,
        vector<MIDIMetaEvent*>* vMetaEvents = nullptr, eventvec_t* vTempo = nullptr, eventvec_t* vSignature = nullptr, eventvec_t* vMarkers = nullptr);
    void ConnectNotes();
    void clear( void );

    friend class MIDIPos;
    friend class MIDITrack;
    friend class MIDIEvent;

    struct MIDIInfo
    {
        MIDIInfo() { clear(); }
        void clear() { llTotalMicroSecs = llFirstNote = iFormatType = iNumTracks = iNumChannels = iDivision = iMinNote =
                       iMaxNote = iNoteCount = iEventCount = iMaxVolume = iVolumeSum = iTotalTicks = iTotalBeats = 0;
                       sFilename.clear(); }
        void AddTrackInfo( const MIDITrack &mTrack);

        wstring sFilename;
        string sMd5;
        uint32_t iFormatType;
        uint32_t iNumTracks, iNumChannels;
        uint32_t iDivision;
        int iMinNote, iMaxNote;
        size_t iNoteCount, iEventCount;
        int iMaxVolume, iVolumeSum;
        int iTotalTicks, iTotalBeats;
        long long llTotalMicroSecs, llFirstNote;
    };

    const MIDIInfo& GetInfo() const { return m_Info; }
    const vector< MIDITrack* >& GetTracks() const { return m_vTracks; }

private:
    struct EventPool {
        MIDIChannelEvent* events;
        size_t count;
    };

    static void InitArrays();
    static wstring aNoteNames[KEYS + 1];
    static Note aNoteVal[KEYS];
    static bool aIsSharp[KEYS];
    static int aWhiteCount[KEYS + 1];

    MIDIInfo m_Info;
    vector< MIDITrack* > m_vTracks;

    std::vector<EventPool> event_pools;
};

//Holds all the event of one MIDI track
class MIDITrack
{
public:
    MIDITrack(MIDI& midi);
    ~MIDITrack( void );

    //Parsing functions that load data into the instance
    size_t ParseTrack( const unsigned char *pcData, size_t iMaxSize, size_t iTrack );
    size_t ParseEvents( const unsigned char *pcData, size_t iMaxSize, size_t iTrack );
    void clear( void );

    friend class MIDIPos;
    friend class MIDI;

    struct MIDITrackInfo
    {
        MIDITrackInfo() { clear(); }
        void clear() { llTotalMicroSecs = iSequenceNumber = iMinNote = iMaxNote = iNoteCount = 
                       iEventCount = iMaxVolume = iVolumeSum = iTotalTicks = iNumChannels = 0;
                       memset( aNoteCount, 0, sizeof( aNoteCount ) ),
                       memset( aProgram, 0, sizeof( aProgram ) ),
                       sSequenceName.clear(); }
        void AddEventInfo( const MIDIEvent &mTrack );

        uint32_t iSequenceNumber;
        string sSequenceName;
        int iMinNote, iMaxNote;
        size_t iNoteCount, iEventCount;
        int iMaxVolume, iVolumeSum;
        int iTotalTicks;
        long long llTotalMicroSecs;
        size_t aNoteCount[16];
        int aProgram[16], iNumChannels;
    };
    const MIDITrackInfo& GetInfo() const { return m_TrackInfo; }
    void ClearEvents() { m_vEvents.clear(); m_vEvents.shrink_to_fit(); }

private:
    MIDITrackInfo m_TrackInfo;
    vector< MIDIEvent* > m_vEvents;
    MIDI& m_MIDI;
};

//Base Event class
//Should really be a single class with unions for the different events. much faster that way.
//Might be forced to convert if batch processing is too slow
class MIDIEvent
{
public:
    //Event types
    enum EventType { ChannelEvent, MetaEvent, SysExEvent, RunningStatus };
    static EventType DecodeEventType( int iEventCode );

    //Parsing functions that load data into the instance
    static int MakeNextEvent( MIDI& midi, const unsigned char *pcData, size_t iMaxSize, int iTrack, MIDIEvent **pOutEvent );

    //Accessors
    EventType GetEventType() const { return (EventType)m_eEventType; }
    unsigned char GetEventCode() const { return m_iEventCode; }
    int GetTrack() const { return m_iTrack; }
    int GetAbsT() const { return m_iAbsT; }
    long long GetAbsMicroSec() const { return m_llAbsMicroSec; }
    void SetAbsMicroSec(long long llAbsMicroSec) { m_llAbsMicroSec = llAbsMicroSec; };

    long long m_llAbsMicroSec;
    int m_iAbsT;
    unsigned short m_iTrack;
    char m_eEventType;
    unsigned char m_iEventCode;
};

//Channel Event: notes and whatnot
class MIDIChannelEvent : public MIDIEvent
{
public:
    MIDIChannelEvent() : m_iSisterIdx(-1), m_iSimultaneous(0), m_bPassDone(false) { }

    enum ChannelEventType { NoteOff = 0x8, NoteOn, NoteAftertouch, Controller, ProgramChange, ChannelAftertouch, PitchBend };
    int ParseEvent( const unsigned char *pcData, size_t iMaxSize );

    //Accessors
    ChannelEventType GetChannelEventType() const { return static_cast<ChannelEventType>(m_iEventCode >> 4); }
    unsigned char GetChannel() const { return m_cChannel; }
    unsigned char GetParam1() const { return m_cParam1; }
    unsigned char GetParam2() const { return m_cParam2; }
    MIDIChannelEvent *GetSister(const std::vector<MIDIChannelEvent*>& events) const {
        return m_iSisterIdx == UINT32_MAX ? nullptr : events[m_iSisterIdx];
    }
    MIDIChannelEvent *GetSister(const std::vector<MIDIEvent*>& events) const {
        return m_iSisterIdx == UINT32_MAX ? nullptr : (MIDIChannelEvent*)events[m_iSisterIdx];
    }
    unsigned GetSisterIdx() const { return m_iSisterIdx; }
    unsigned GetSimultaneous() const { return m_iSimultaneous; }
    unsigned GetLength() const { return m_uLength; }
    bool GetPassDone() const { return m_bPassDone; }

    void SetChannel(unsigned char channel) { m_cChannel = channel; }
    void SetParam1(unsigned char param1) { m_cParam1 = param1; }
    void SetParam2(unsigned char param2) { m_cParam2 = param2; }
    void SetSisterIdx(unsigned iSisterIdx) { m_iSisterIdx = iSisterIdx; }
    void SetSimultaneous(int iSimultaneous) { m_iSimultaneous = iSimultaneous; }
    void SetLength(unsigned length) { m_uLength = length; }
    void SetPassDone(bool done) { m_bPassDone = done; }

    bool HasSister() const { return m_iSisterIdx != UINT32_MAX; }

private:
    unsigned m_iSisterIdx;
    unsigned m_iSimultaneous;
    unsigned m_uLength;
    bool m_bPassDone;
    unsigned char m_cChannel;
    unsigned char m_cParam1;
    unsigned char m_cParam2;
};

//Meta Event: info about the notes and whatnot
class MIDIMetaEvent : public MIDIEvent
{
public:
    MIDIMetaEvent() : m_pcData( 0 ) { }
    ~MIDIMetaEvent() { if ( m_pcData ) delete[] m_pcData; }

    enum MetaEventType { SequenceNumber, TextEvent, Copyright, SequenceName, InstrumentName, Lyric, Marker,
                         CuePoint, ChannelPrefix = 0x20, PortPrefix = 0x21, EndOfTrack = 0x2F, SetTempo = 0x51,
                         SMPTEOffset = 0x54, TimeSignature = 0x58, KeySignature = 0x59, Proprietary = 0x7F };
    int ParseEvent( const unsigned char *pcData, size_t iMaxSize );

    //Accessors
    MetaEventType GetMetaEventType() const { return m_eMetaEventType; }
    int GetDataLen() const { return m_iDataLen; }
    unsigned char *GetData() const { return m_pcData; }

private:
    MetaEventType m_eMetaEventType;
    uint32_t m_iDataLen;
    unsigned char *m_pcData;
};

//SysEx Event: probably to be ignored
class MIDISysExEvent : public MIDIEvent
{
public:
    MIDISysExEvent() : m_pcData( 0 ) { }
    ~MIDISysExEvent() { if ( m_pcData ) delete[] m_pcData; }

    int ParseEvent( const unsigned char *pcData, size_t iMaxSize );

private:
    uint32_t m_iDataLen;
    unsigned char *m_pcData;
    bool m_bHasMoreData;
};

//
// MIDI Device Classes
//

class MIDIOutDevice
{
public:
    MIDIOutDevice() : m_bIsOpen(false), m_hMIDIOut( NULL ) { }
    virtual ~MIDIOutDevice() { Close(); }

    int GetNumDevs() const;
    wstring GetDevName( int iDev ) const;
    bool Open( int iDev );
    bool OpenKDMAPI();
    void Close();
    void Reset();

    bool IsOpen() const { return m_bIsOpen; }
    const wstring& GetDevice() const { return m_sDevice; };

    void AllNotesOff();
    void AllNotesOff( const vector< int > &vChannels );
    void SetVolume( double dVolume );

    bool PlayEventAcrossChannels( unsigned char cStatus, unsigned char cParam1, unsigned char cParam2 );
    bool PlayEventAcrossChannels( unsigned char cStatus, unsigned char cParam1, unsigned char cParam2, const vector< int > &vChannels );
    bool PlayEvent( unsigned char bStatus, unsigned char bParam1, unsigned char bParam2 = 0 );

    bool IsKDMAPIEnabled() { return m_bIsKDMAPI; }
    float KDMAPIRenderingTime();
    uint64_t KDMAPIActiveVoices();

private:
    static FARPROC GetOmniMIDIProc(const char* func);
    static void CALLBACK MIDIOutProc( HMIDIOUT hmo, UINT wMsg, DWORD_PTR dwInstance,
                                      DWORD_PTR dwParam1, DWORD_PTR dwParam2 );

    bool m_bIsOpen;
    bool m_bIsKDMAPI;
    void(WINAPI* SendDirectData)(DWORD);
    float(WINAPI* GetRenderingTime)();
    uint64_t(WINAPI* GetVoiceCount)();
    wstring m_sDevice;
    HMIDIOUT m_hMIDIOut;
};

class MIDILoadingProgress {
public:
    enum Stage { CopyToMem, Decompress, ParseTracks, ConnectNotes, SortEvents, Finalize, Done };

    Stage stage;
    std::wstring name;
    std::atomic<uint64_t> progress;
    uint64_t max;
};

extern MIDILoadingProgress g_LoadingProgress;