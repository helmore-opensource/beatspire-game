/* This stores a single note pattern for a song.
 *
 * We can have too much data to keep everything decompressed as NoteData, so most
 * songs are kept in memory compressed as SMData until requested.  NoteData is normally
 * not requested casually during gameplay; we can move through screens, the music
 * wheel, etc. without touching any NoteData.
 *
 * To save more memory, if data is cached on disk, read it from disk on demand.  Not
 * all Steps will have an associated file for this purpose.  (Profile edits don't do
 * this yet.)
 *
 * Data can be on disk (always compressed), compressed in memory, and uncompressed in
 * memory. */
#include "global.h"
#include "Steps.h"
#include "StepsUtil.h"
#include "GameState.h"
#include "Song.h"
#include "RageUtil.h"
#include "RageLog.h"
#include "NoteData.h"
#include "GameManager.h"
#include "SongManager.h"
#include "NoteDataUtil.h"
#include "NotesLoaderSSC.h"
#include "NotesLoaderSM.h"
#include "NotesLoaderSMA.h"
#include "NotesLoaderDWI.h"
#include "NotesLoaderKSF.h"
#include "NotesLoaderBMS.h"
#include <algorithm>
#include "MinaCalc.h"
#include <thread>

/* register DisplayBPM with StringConversion */
#include "EnumHelper.h"

// For hashing wife chart keys - Mina
#include "CryptManager.h"

static const char *DisplayBPMNames[] =
{
	"Actual",
	"Specified",
	"Random",
};
XToString( DisplayBPM );
LuaXType( DisplayBPM );

Steps::Steps(Song *song): m_StepsType(StepsType_Invalid), m_pSong(song),
	parent(NULL), m_pNoteData(new NoteData), m_bNoteDataIsFilled(false), 
	m_sNoteDataCompressed(""), m_sFilename(""), m_bSavedToDisk(false), 
	m_LoadedFromProfile(ProfileSlot_Invalid), m_iHash(0),
	m_sDescription(""), m_sChartStyle(""), 
	m_Difficulty(Difficulty_Invalid), m_iMeter(0),
	m_bAreCachedRadarValuesJustLoaded(false),
	m_sCredit(""), displayBPMType(DISPLAY_BPM_ACTUAL),
	specifiedBPMMin(0), specifiedBPMMax(0) {}

Steps::~Steps()
{
}

void Steps::GetDisplayBpms( DisplayBpms &AddTo ) const
{
	if( this->GetDisplayBPM() == DISPLAY_BPM_SPECIFIED )
	{
		AddTo.Add( this->GetMinBPM() );
		AddTo.Add( this->GetMaxBPM() );
	}
	else
	{
		float fMinBPM, fMaxBPM;
		this->GetTimingData()->GetActualBPM( fMinBPM, fMaxBPM );
		AddTo.Add( fMinBPM );
		AddTo.Add( fMaxBPM );
	}
}

bool Steps::HasAttacks() const
{
	return !this->m_Attacks.empty();
}

unsigned Steps::GetHash() const
{
	if( parent )
		return parent->GetHash();
	if( m_iHash )
		return m_iHash;
	if( m_sNoteDataCompressed.empty() )
	{
		if( !m_bNoteDataIsFilled )
			return 0; // No data, no hash.
		NoteDataUtil::GetSMNoteDataString( *m_pNoteData, m_sNoteDataCompressed );
	}
	m_iHash = GetHashForString( m_sNoteDataCompressed );
	return m_iHash;
}

bool Steps::IsNoteDataEmpty() const
{
	return this->m_sNoteDataCompressed.empty();
}

bool Steps::GetNoteDataFromSimfile()
{
	// Replace the line below with the Steps' cache file.
	RString stepFile = this->GetFilename();
	RString extension = GetExtension(stepFile);
	extension.MakeLower(); // must do this because the code is expecting lowercase

	if (extension.empty() || extension == "ssc"
		|| extension == "ats") // remember cache files.
	{
		SSCLoader loader;
		if ( ! loader.LoadNoteDataFromSimfile(stepFile, *this) )
		{
			/*
			HACK: 7/20/12 -- see bugzilla #740
			users who edit songs using the ever popular .sm file
			that remove or tamper with the .ssc file later on 
			complain of blank steps in the editor after reloading.
			Despite the blank steps being well justified since 
			the cache files contain only the SSC step file,
			give the user some leeway and search for a .sm replacement
			*/
			SMLoader backup_loader;
			RString transformedStepFile = stepFile;
			transformedStepFile.Replace(".ssc", ".sm");
			
			return backup_loader.LoadNoteDataFromSimfile(transformedStepFile, *this);
		}
		else
		{
			return true;
		}
	}
	else if (extension == "sm")
	{
		SMLoader loader;
		return loader.LoadNoteDataFromSimfile(stepFile, *this);
	}
	else if (extension == "sma")
	{
		SMALoader loader;
		return loader.LoadNoteDataFromSimfile(stepFile, *this);
	}
	else if (extension == "dwi")
	{
		return DWILoader::LoadNoteDataFromSimfile(stepFile, *this);
	}
	else if (extension == "ksf")
	{
		return KSFLoader::LoadNoteDataFromSimfile(stepFile, *this);
	}
	else if (extension == "bms" || extension == "bml" || extension == "bme" || extension == "pms")
	{
		return BMSLoader::LoadNoteDataFromSimfile(stepFile, *this);
	}
	else if (extension == "edit")
	{
		// Try SSC, then fallback to SM.
		SSCLoader ldSSC;
		if(ldSSC.LoadNoteDataFromSimfile(stepFile, *this) != true)
		{
			SMLoader ldSM;
			return ldSM.LoadNoteDataFromSimfile(stepFile, *this);
		}
		else return true;
	}
	return false;
}

void Steps::SetNoteData( const NoteData& noteDataNew )
{
	ASSERT( noteDataNew.GetNumTracks() == GAMEMAN->GetStepsTypeInfo(m_StepsType).iNumTracks );

	DeAutogen( false );

	*m_pNoteData = noteDataNew;
	m_bNoteDataIsFilled = true;

	m_sNoteDataCompressed = RString();
	m_iHash = 0;
}

void Steps::GetNoteData( NoteData& noteDataOut, bool isGameplay ) const
{
	// HACK: isGameplay breaks the logic for not decompressing when already decompressed
	// because decompress may have been run by a false before hand
	if( isGameplay )
		Compress();

	Decompress(isGameplay);

	if( m_bNoteDataIsFilled )
	{
		noteDataOut = *m_pNoteData;
	}
	else
	{
		noteDataOut.ClearAll();
		noteDataOut.SetNumTracks( GAMEMAN->GetStepsTypeInfo(m_StepsType).iNumTracks );
	}
}

NoteData Steps::GetNoteData() const
{
	NoteData tmp;
	this->GetNoteData( tmp, false );
	return tmp;
}

void Steps::SetSMNoteData( const RString &notes_comp_ )
{
	m_pNoteData->Init();
	m_bNoteDataIsFilled = false;

	m_sNoteDataCompressed = notes_comp_;
	m_iHash = 0;
}

/* XXX: this function should pull data from m_sFilename, like Decompress() */
void Steps::GetSMNoteData( RString &notes_comp_out ) const
{
	if( m_sNoteDataCompressed.empty() )
	{
		if( !m_bNoteDataIsFilled ) 
		{
			/* no data is no data */
			notes_comp_out = "";
			return;
		}

		NoteDataUtil::GetSMNoteDataString( *m_pNoteData, m_sNoteDataCompressed );
	}

	notes_comp_out = m_sNoteDataCompressed;
}

float Steps::PredictMeter() const
{
	float pMeter = 0.775f;

	const float RadarCoeffs[NUM_RadarCategory] =
	{
		10.1f, 5.27f,-0.905f, -1.10f, 2.86f,
		0,0,0,0,0,0,0,0
	};
	const RadarValues &rv = GetRadarValues( PLAYER_1 );
	for( int r = 0; r < NUM_RadarCategory; ++r )
		pMeter += rv[r] * RadarCoeffs[r];

	const float DifficultyCoeffs[NUM_Difficulty] =
	{
		-0.877f, -0.877f, 0, 0.722f, 0.722f, 0
	};
	pMeter += DifficultyCoeffs[this->GetDifficulty()];

	// Init non-radar values
	const float SV = rv[RadarCategory_Stream] * rv[RadarCategory_Voltage];
	const float ChaosSquare = rv[RadarCategory_Chaos] * rv[RadarCategory_Chaos];
	pMeter += -6.35f * SV;
	pMeter += -2.58f * ChaosSquare;
	if (pMeter < 1) pMeter = 1;
	return pMeter;
}

void Steps::TidyUpData()
{
	// Don't set the StepsType to dance single if it's invalid.  That just
	// causes unrecognized charts to end up where they don't belong.
	// Leave it as StepsType_Invalid so the Song can handle it specially.  This
	// is a forwards compatibility feature, so that if a future version adds a
	// new style, editing a simfile with unrecognized Steps won't silently
	// delete them. -Kyz
	if( m_StepsType == StepsType_Invalid )
	{
		LOG->Warn("Detected steps with unknown style '%s' in '%s'", m_StepsTypeStr.c_str(), m_pSong->m_sSongFileName.c_str());
	}
	else if(m_StepsTypeStr == "")
	{
		m_StepsTypeStr= GAMEMAN->GetStepsTypeInfo(m_StepsType).szName;
	}

	if( GetDifficulty() == Difficulty_Invalid )
		SetDifficulty( StringToDifficulty(GetDescription()) );

	if( GetDifficulty() == Difficulty_Invalid )
	{
		if(	 GetMeter() == 1 )	SetDifficulty( Difficulty_Beginner );
		else if( GetMeter() <= 3 )	SetDifficulty( Difficulty_Easy );
		else if( GetMeter() <= 6 )	SetDifficulty( Difficulty_Medium );
		else				SetDifficulty( Difficulty_Hard );
	}

	if( GetMeter() < 1) // meter is invalid
		SetMeter(static_cast<int>(PredictMeter()) );
}

void Steps::CalculateRadarValues( float fMusicLengthSeconds )
{
	// If we're autogen, don't calculate values.  GetRadarValues will take from our parent.
	if( parent != NULL )
		return;

	if( m_bAreCachedRadarValuesJustLoaded )
	{
		m_bAreCachedRadarValuesJustLoaded = false;
		return;
	}

	// Do write radar values, and leave it up to the reading app whether they want to trust
	// the cached values without recalculating them.
	/*
	// If we're an edit, leave the RadarValues invalid.
	if( IsAnEdit() )
		return;
	*/
	
	NoteData tempNoteData;
	this->GetNoteData( tempNoteData, true );

	FOREACH_PlayerNumber( pn )
		m_CachedRadarValues[pn].Zero();

	GAMESTATE->SetProcessedTimingData(this->GetTimingData());
	if( tempNoteData.IsComposite() )
	{
		vector<NoteData> vParts;

		NoteDataUtil::SplitCompositeNoteData( tempNoteData, vParts );
		for( size_t pn = 0; pn < min(vParts.size(), size_t(NUM_PLAYERS)); ++pn )
			NoteDataUtil::CalculateRadarValues( vParts[pn], fMusicLengthSeconds, m_CachedRadarValues[pn] );
	}
	else if (GAMEMAN->GetStepsTypeInfo(this->m_StepsType).m_StepsTypeCategory == StepsTypeCategory_Couple)
	{
		NoteData p1 = tempNoteData;
		// XXX: Assumption that couple will always have an even number of notes.
		const int tracks = tempNoteData.GetNumTracks() / 2;
		p1.SetNumTracks(tracks);
		NoteDataUtil::CalculateRadarValues(p1,
										   fMusicLengthSeconds,
										   m_CachedRadarValues[PLAYER_1]);
		// at this point, p2 is tempNoteData.
		NoteDataUtil::ShiftTracks(tempNoteData, tracks);
		tempNoteData.SetNumTracks(tracks);
		NoteDataUtil::CalculateRadarValues(tempNoteData,
										   fMusicLengthSeconds,
										   m_CachedRadarValues[PLAYER_2]);
	}
	else
	{
		NoteDataUtil::CalculateRadarValues( tempNoteData, fMusicLengthSeconds, m_CachedRadarValues[0] );
		fill_n( m_CachedRadarValues + 1, NUM_PLAYERS-1, m_CachedRadarValues[0] );
	}
	GAMESTATE->SetProcessedTimingData(NULL);
}

void Steps::Decompress(bool isGameplay) const
{
	const_cast<Steps *>(this)->Decompress(isGameplay);
}

bool stepstype_is_kickbox(StepsType st)
{
	return st == StepsType_kickbox_human || st == StepsType_kickbox_quadarm ||
		st == StepsType_kickbox_insect || st == StepsType_kickbox_arachnid;
}

void Steps::Decompress(bool isGameplay)
{

	if (m_bNoteDataIsFilled) {
		return;	// already decompressed
	}
		
	if( parent )
	{
		// get autogen m_pNoteData
		NoteData notedata;
		parent->GetNoteData( notedata, isGameplay );

		m_bNoteDataIsFilled = true;

		int iNewTracks = GAMEMAN->GetStepsTypeInfo(m_StepsType).iNumTracks;

		if( this->m_StepsType == StepsType_lights_cabinet )
		{
			NoteDataUtil::LoadTransformedLights( notedata, *m_pNoteData, iNewTracks );
		}
		else
		{
			// Special case so that kickbox can have autogen steps that are playable.
			// Hopefully I'll replace this with a good generalized autogen system
			// later.  -Kyz
			if(stepstype_is_kickbox(this->m_StepsType))
			{
				// Number of notes seems like a useful "random" input so that charts
				// from different sources come out different, but autogen always
				// makes the same thing from one source. -Kyz
				NoteDataUtil::AutogenKickbox(notedata, *m_pNoteData, *GetTimingData(),
					this->m_StepsType,
					static_cast<int>(GetRadarValues(PLAYER_1)[RadarCategory_TapsAndHolds]));
			}
			else
			{
				NoteDataUtil::LoadTransformedSlidingWindow( notedata, *m_pNoteData, iNewTracks );

				NoteDataUtil::RemoveStretch( *m_pNoteData, m_StepsType );
			}
		}
		return;
	}

	if( !m_sFilename.empty() && m_sNoteDataCompressed.empty() )
	{
		// We have NoteData on disk and not in memory. Load it.
		if (!this->GetNoteDataFromSimfile())
		{
			LOG->Warn("Couldn't load the %s chart's NoteData from \"%s\"",
					  DifficultyToString(m_Difficulty).c_str(), m_sFilename.c_str());
			return;
		}

		this->GetSMNoteData( m_sNoteDataCompressed );
	}

	if( m_sNoteDataCompressed.empty() )
	{
		/* there is no data, do nothing */
		return;
	}
	else
	{
		// load from compressed
		bool bComposite = GAMEMAN->GetStepsTypeInfo(m_StepsType).m_StepsTypeCategory == StepsTypeCategory_Routine;
		m_bNoteDataIsFilled = true;
		m_pNoteData->SetNumTracks(GAMEMAN->GetStepsTypeInfo(m_StepsType).iNumTracks);
		NoteDataUtil::LoadFromSMNoteDataString(*m_pNoteData, m_sNoteDataCompressed, bComposite);
	}
	/*	Piggy backing off this function to generate stuff that should only be generated once per song caching
	This includes a vector for note rows that are not empty, a vector for elapsed time at each note row and
	the chart key generated on song load. Also this totally needs to be redone and split up into the appropriate
	places now that I understand the system better. - Mina */

	if (isGameplay)
	{
		TimingData *td = Steps::GetTimingData();
		vector<int>& nerv = m_pNoteData->GetNonEmptyRowVector();
		vector<float> etaner;

		for (size_t i = 0; i < nerv.size(); i++)
			etaner.push_back(td->GetElapsedTimeFromBeatNoOffset(NoteRowToBeat(nerv[i])));
		
		ChartKey = GenerateChartKey(*m_pNoteData, td);
		stuffnthings = MinaSDCalc(m_pNoteData->SerializeNoteData(etaner), m_pNoteData->GetNumTracks(), 0.93f, 1.f, td->HasWarps());
	}
}

bool Steps::IsRecalcValid() {
	if (m_StepsType != StepsType_dance_single)
		return false;

	TimingData* td = GetTimingData();
	if (td->HasWarps())
		return false;

	return true;
}

float Steps::GetMSD(float x, int i) const {
	int idx = static_cast<int>(x * 10) - 7;
	CLAMP(idx, 0, 13);	// prevent crashes due to people setting rate mod below 0.7 or above 2.0 somehow - mina
	float prop = fmod(x * 10.f, 1.f);
	if ( prop == 0)
		return stuffnthings[idx][i];
	return lerp(prop, stuffnthings[idx][i], stuffnthings[idx + 1][i]);
}

map<float, Skillset> Steps::SortSkillsetsAtRate(float x, bool includeoverall) {
	int idx = static_cast<int>(x * 10) - 7;
	map<float, Skillset> why;
	SDiffs tmp = stuffnthings[idx];
	FOREACH_ENUM(Skillset, ss)
		if(ss != Skill_Overall || includeoverall)
			why.emplace(tmp[ss], ss);
	return why;
}

RString Steps::GenerateChartKey(NoteData& nd, TimingData *td)
{
	RString k = "";
	RString o = "";
	vector<int>& nerv = nd.GetNonEmptyRowVector();
	
	unsigned int numThreads = max(std::thread::hardware_concurrency(), 1u);
	std::vector<RString> keyParts;
	keyParts.reserve(numThreads);
	
	size_t segmentSize = nerv.size() / numThreads;
	std::vector<std::thread> threads;
	threads.reserve(numThreads);
	
	for (unsigned int curThread = 0; curThread < numThreads; curThread++)
	{
		keyParts.push_back("");
		size_t start = segmentSize * curThread;
		size_t end = start + segmentSize;
		if (curThread + 1 == numThreads)
			end = nerv.size();

		threads.push_back(std::thread(&Steps::FillStringWithBPMs, this, start, end, std::ref(nerv), std::ref(nd), td, std::ref(keyParts[curThread])));
	}

	for (auto& t : threads)
	{
		if(t.joinable())
			t.join();
	}

	for(size_t i = 0; i < numThreads; i++)
		k += keyParts[i];

	o.append("X");	// I was thinking of using "C" to indicate chart.. however.. X is cooler... - Mina
	o.append(BinaryToHex(CryptManager::GetSHA1ForString(k)));
	return o;
}

void Steps::FillStringWithBPMs(size_t startRow, size_t endRow, vector<int>& nerv, NoteData& nd, TimingData *td, RString& inOut)
{
	float bpm = 0.f;
	for (size_t r = startRow; r < endRow; r++) {
		int row = nerv[r];
		for (int t = 0; t < nd.GetNumTracks(); ++t) {
			const TapNote& tn = nd.GetTapNote(t, row);
			inOut.append(to_string(tn.type));
		}
		bpm = td->GetBPMAtRow(row);
		inOut.append(to_string(static_cast<int>(bpm + 0.374643f)));
	}
}

int Steps::GetNPSVector(NoteData nd, vector<float>& etar)
{
	vector<vector<int>> doot;
	vector<int> scoot;
	int intN = 1;
	float intI = 0.5f;
	int intT = 0;
	vector<int> intervaltaps;
	vector<int>& nerv = nd.GetNonEmptyRowVector();

	for (size_t r = 0; r < nerv.size(); r++)
	{
		int row = nerv[r];
		if (etar[row] >= intN * intI) {
			doot.push_back(scoot);
			scoot.clear();
			intN += 1;

			intervaltaps.push_back(intT / intI);
			intT = 0;
		}
		scoot.push_back(row);
		for (int t = 0; t < nd.GetNumTracks(); ++t)
		{
			const TapNote &tn = nd.GetTapNote(t, row);
			if (tn.type == TapNoteType_Tap || tn.type == TapNoteType_HoldHead) {
				intT += 1;
			}

		}
	}
	return 1;
}

void Steps::Compress() const
{
	// Always leave lights data uncompressed.
	if( this->m_StepsType == StepsType_lights_cabinet && m_bNoteDataIsFilled )
	{
		m_sNoteDataCompressed = RString();
		return;
	}
	
	// Don't compress data in the editor: it's still in use.
	if (GAMESTATE->m_bInStepEditor)
	{
		return;
	}

	if( !m_sFilename.empty() && m_LoadedFromProfile == ProfileSlot_Invalid )
	{
		/* We have a file on disk; clear all data in memory.
		 * Data on profiles can't be accessed normally (need to mount and time-out
		 * the device), and when we start a game and load edits, we want to be
		 * sure that it'll be available if the user picks it and pulls the device.
		 * Also, Decompress() doesn't know how to load .edits. */
		m_pNoteData->Init();
		m_bNoteDataIsFilled = false;

		/* Be careful; 'x = ""', m_sNoteDataCompressed.clear() and m_sNoteDataCompressed.reserve(0)
		 * don't always free the allocated memory. */
		m_sNoteDataCompressed = RString();
		return;
	}

	// We have no file on disk. Compress the data, if necessary.
	if( m_sNoteDataCompressed.empty() )
	{
		if( !m_bNoteDataIsFilled )
			return; /* no data is no data */
		NoteDataUtil::GetSMNoteDataString( *m_pNoteData, m_sNoteDataCompressed );
	}

	m_pNoteData->Init();
	m_bNoteDataIsFilled = false;
}

/* Copy our parent's data. This is done when we're being changed from autogen
 * to normal. (needed?) */
void Steps::DeAutogen( bool bCopyNoteData )
{
	if( !parent )
		return; // OK

	if( bCopyNoteData )
		Decompress(false);	// fills in m_pNoteData with sliding window transform

	m_sDescription		= Real()->m_sDescription;
	m_sChartStyle		= Real()->m_sChartStyle;
	m_Difficulty		= Real()->m_Difficulty;
	m_iMeter		= Real()->m_iMeter;
	copy( Real()->m_CachedRadarValues, Real()->m_CachedRadarValues + NUM_PLAYERS, m_CachedRadarValues );
	m_sCredit		= Real()->m_sCredit;
	parent = NULL;

	if( bCopyNoteData )
		Compress();
}

void Steps::AutogenFrom( const Steps *parent_, StepsType ntTo )
{
	parent = parent_;
	m_StepsType = ntTo;
	m_StepsTypeStr= GAMEMAN->GetStepsTypeInfo(ntTo).szName;
	m_Timing = parent->m_Timing;
}

void Steps::CopyFrom( Steps* pSource, StepsType ntTo, float fMusicLengthSeconds )	// pSource does not have to be of the same StepsType
{
	m_StepsType = ntTo;
	m_StepsTypeStr= GAMEMAN->GetStepsTypeInfo(ntTo).szName;
	NoteData noteData;
	pSource->GetNoteData( noteData, false );
	noteData.SetNumTracks( GAMEMAN->GetStepsTypeInfo(ntTo).iNumTracks );
	parent = NULL;
	m_Timing = pSource->m_Timing;
	this->m_pSong = pSource->m_pSong;
	this->m_Attacks = pSource->m_Attacks;
	this->m_sAttackString = pSource->m_sAttackString;
	this->SetNoteData( noteData );
	this->SetDescription( pSource->GetDescription() );
	this->SetDifficulty( pSource->GetDifficulty() );
	this->SetMeter( pSource->GetMeter() );
	this->CalculateRadarValues( fMusicLengthSeconds );
}

void Steps::CreateBlank( StepsType ntTo )
{
	m_StepsType = ntTo;
	m_StepsTypeStr= GAMEMAN->GetStepsTypeInfo(ntTo).szName;
	NoteData noteData;
	noteData.SetNumTracks( GAMEMAN->GetStepsTypeInfo(ntTo).iNumTracks );
	this->SetNoteData( noteData );
}

void Steps::SetDifficultyAndDescription( Difficulty dc, const RString &sDescription )
{
	DeAutogen();
	m_Difficulty = dc;
	m_sDescription = sDescription;
	if( GetDifficulty() == Difficulty_Edit )
		MakeValidEditDescription( m_sDescription );
}

void Steps::SetCredit( const RString &sCredit )
{
	DeAutogen();
	m_sCredit = sCredit;
}

void Steps::SetChartStyle( const RString &sChartStyle )
{
	DeAutogen();
	m_sChartStyle = sChartStyle;
}

bool Steps::MakeValidEditDescription( RString &sPreferredDescription )
{
	if( static_cast<int>(sPreferredDescription.size()) > MAX_STEPS_DESCRIPTION_LENGTH )
	{
		sPreferredDescription = sPreferredDescription.Left( MAX_STEPS_DESCRIPTION_LENGTH );
		return true;
	}
	return false;
}

void Steps::SetMeter( int meter )
{
	DeAutogen();
	m_iMeter = meter;
}

const TimingData *Steps::GetTimingData() const
{
	return m_Timing.empty() ? &m_pSong->m_SongTiming : &m_Timing;
}

bool Steps::HasSignificantTimingChanges() const
{
	const TimingData *timing = GetTimingData();
	if( timing->HasStops() || timing->HasDelays() || timing->HasWarps() ||
		timing->HasSpeedChanges() || timing->HasScrollChanges() )
		return true;

	if( timing->HasBpmChanges() )
	{
		// check to see if these changes are significant.
		if( (GetMaxBPM() - GetMinBPM()) > 3.000f )
			return true;
	}

	return false;
}

const RString Steps::GetMusicPath() const
{
	return Song::GetSongAssetPath(
		m_MusicFile.empty() ? m_pSong->m_sMusicFile : m_MusicFile,
		m_pSong->GetSongDir());
}

const RString& Steps::GetMusicFile() const
{
	return m_MusicFile;
}

void Steps::SetMusicFile(const RString& file)
{
	m_MusicFile= file;
}

void Steps::SetCachedRadarValues( const RadarValues v[NUM_PLAYERS] )
{
	DeAutogen();
	copy( v, v + NUM_PLAYERS, m_CachedRadarValues );
	m_bAreCachedRadarValuesJustLoaded = true;
}

RString Steps::GetChartKeyRecord() const {
	return ChartKeyRecord;
};

// lua start
#include "LuaBinding.h"
/** @brief Allow Lua to have access to the Steps. */
class LunaSteps: public Luna<Steps>
{
public:
	DEFINE_METHOD( GetStepsType,	m_StepsType )
	DEFINE_METHOD( GetDifficulty,	GetDifficulty() )
	DEFINE_METHOD( GetDescription,	GetDescription() )
	DEFINE_METHOD( GetChartStyle,	GetChartStyle() )
	DEFINE_METHOD( GetAuthorCredit, GetCredit() )
	DEFINE_METHOD( GetMeter,	GetMeter() )
	DEFINE_METHOD( GetFilename,	GetFilename() )
	DEFINE_METHOD( IsAutogen,	IsAutogen() )
	DEFINE_METHOD( IsAnEdit,	IsAnEdit() )
	DEFINE_METHOD( IsAPlayerEdit,	IsAPlayerEdit() )

	static int HasSignificantTimingChanges( T* p, lua_State *L )
	{
		lua_pushboolean(L, p->HasSignificantTimingChanges()); 
		return 1; 
	}
	static int HasAttacks( T* p, lua_State *L )
	{ 
		lua_pushboolean(L, p->HasAttacks()); 
		return 1; 
	}
	static int GetRadarValues( T* p, lua_State *L )
	{
		PlayerNumber pn = PLAYER_1;
		if (!lua_isnil(L, 1)) {
			pn = Enum::Check<PlayerNumber>(L, 1);
		}
		
		RadarValues &rv = const_cast<RadarValues &>(p->GetRadarValues(pn));
		rv.PushSelf(L);
		return 1;
	}
	// Sigh -Mina
	static int GetRelevantRadars(T* p, lua_State *L)
	{
		vector<int> relevants;
		PlayerNumber pn = PLAYER_1;
		if (!lua_isnil(L, 1))
			pn = Enum::Check<PlayerNumber>(L, 1);

		RadarValues &rv = const_cast<RadarValues &>(p->GetRadarValues(pn));
		relevants.push_back(static_cast<int>(rv[5]));  //notes
		relevants.push_back(static_cast<int>(rv[7]));  //jumps
		relevants.push_back(static_cast<int>(rv[10])); //hands
		relevants.push_back(static_cast<int>(rv[8]));  //holds
		relevants.push_back(static_cast<int>(rv[9]));  //mines
		relevants.push_back(static_cast<int>(rv[11])); //rolls
		relevants.push_back(static_cast<int>(rv[12])); //lifts
		relevants.push_back(static_cast<int>(rv[13])); //fakes
		LuaHelpers::CreateTableFromArray(relevants, L);
		return 1;
	}
	static int GetTimingData( T* p, lua_State *L )
	{
		p->GetTimingData()->PushSelf(L);
		return 1;
	}
	static int GetHash( T* p, lua_State *L ) { lua_pushnumber( L, p->GetHash() ); return 1; }
	// untested
	/*
	static int GetSMNoteData( T* p, lua_State *L )
	{
		RString out;
		p->GetSMNoteData( out );
		lua_pushstring( L, out );
		return 1;
	}
	*/
	static int GetChartName(T *p, lua_State *L)
	{
		lua_pushstring(L, p->GetChartName());
		return 1;
	}
	static int GetDisplayBpms( T* p, lua_State *L )
	{
		DisplayBpms temp;
		p->GetDisplayBpms(temp);
		float fMin = temp.GetMin();
		float fMax = temp.GetMax();
		vector<float> fBPMs;
		fBPMs.push_back( fMin );
		fBPMs.push_back( fMax );
		LuaHelpers::CreateTableFromArray(fBPMs, L);
		return 1;
	}
	static int IsDisplayBpmSecret( T* p, lua_State *L )
	{
		DisplayBpms temp;
		p->GetDisplayBpms(temp);
		lua_pushboolean( L, temp.IsSecret() );
		return 1;
	}
	static int IsDisplayBpmConstant( T* p, lua_State *L )
	{
		DisplayBpms temp;
		p->GetDisplayBpms(temp);
		lua_pushboolean( L, temp.BpmIsConstant() );
		return 1;
	}
	static int IsDisplayBpmRandom( T* p, lua_State *L )
	{
		lua_pushboolean( L, p->GetDisplayBPM() == DISPLAY_BPM_RANDOM );
		return 1;
	}
	DEFINE_METHOD( PredictMeter, PredictMeter() )
	static int GetDisplayBPMType( T* p, lua_State *L )
	{
		LuaHelpers::Push( L, p->GetDisplayBPM() );
		return 1;
	}	

	static int GetChartKey(T* p, lua_State *L) {
		lua_pushstring(L, p->GetChartKey());
		return 1;
	}

	static int GetMSD(T* p, lua_State *L) {
		float rate = FArg(1);
		int index = IArg(2)-1;
		CLAMP(rate, 0.7f, 2.f);
		lua_pushnumber(L, p->GetMSD(rate, index));
		return 1;
	}
	// ok really is this how i have to do this - mina
	static int GetRelevantSkillsetsByMSDRank(T* p, lua_State *L) {
		float rate = FArg(1);
		CLAMP(rate, 0.7f, 2.f);
		auto sortedskillsets = p->SortSkillsetsAtRate(rate, false);
		int rank = IArg(2);
		int i = NUM_Skillset - 1; // exclude Overall from this... need to handle overall better - mina
		Skillset o;
		float rval = 0.f;
		float highval = 0.f;
		FOREACHM(float, Skillset, sortedskillsets, thingy) {				
			if (i == rank) {
				rval = thingy->first;
				o = thingy->second;
			}
			if (i == 1)
				highval = thingy->first;
			--i;
		}
		if(rval > highval * 0.9f)
			lua_pushstring(L, SkillsetToString(o) );
		else
			lua_pushstring(L, "");
		return 1;
	}

	LunaSteps()
	{
		ADD_METHOD( GetAuthorCredit );
		ADD_METHOD( GetChartStyle );
		ADD_METHOD( GetDescription );
		ADD_METHOD( GetDifficulty );
		ADD_METHOD( GetFilename );
		ADD_METHOD( GetHash );
		ADD_METHOD( GetMeter );
		ADD_METHOD( HasSignificantTimingChanges );
		ADD_METHOD( HasAttacks );
		ADD_METHOD( GetRadarValues );
		ADD_METHOD( GetRelevantRadars );
		ADD_METHOD( GetTimingData );
		ADD_METHOD( GetChartName );
		//ADD_METHOD( GetSMNoteData );
		ADD_METHOD( GetStepsType );
		ADD_METHOD( GetChartKey );
		ADD_METHOD( GetMSD );
		ADD_METHOD( IsAnEdit );
		ADD_METHOD( IsAutogen );
		ADD_METHOD( IsAPlayerEdit );
		ADD_METHOD( GetDisplayBpms );
		ADD_METHOD( IsDisplayBpmSecret );
		ADD_METHOD( IsDisplayBpmConstant );
		ADD_METHOD( IsDisplayBpmRandom );
		ADD_METHOD( PredictMeter );
		ADD_METHOD( GetDisplayBPMType );
		ADD_METHOD( GetRelevantSkillsetsByMSDRank );
	}
};

LUA_REGISTER_CLASS( Steps )
// lua end


/*
 * (c) 2001-2004 Chris Danford, Glenn Maynard, David Wilson
 * All rights reserved.
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, and/or sell copies of the Software, and to permit persons to
 * whom the Software is furnished to do so, provided that the above
 * copyright notice(s) and this permission notice appear in all copies of
 * the Software and that both the above copyright notice(s) and this
 * permission notice appear in supporting documentation.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF
 * THIRD PARTY RIGHTS. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR HOLDERS
 * INCLUDED IN THIS NOTICE BE LIABLE FOR ANY CLAIM, OR ANY SPECIAL INDIRECT
 * OR CONSEQUENTIAL DAMAGES, OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */
