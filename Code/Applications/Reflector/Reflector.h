#pragma once

#include "Applications/Reflector/Database/ReflectionDatabase.h"
#include "System/Time/Time.h"
#include "System/Types/String.h"

//-------------------------------------------------------------------------

namespace EE::TypeSystem::Reflection
{
    class Reflector
    {
        enum class HeaderProcessResult
        {
            ErrorOccured,
            ParseHeader,
            IgnoreHeader,
        };

        struct HeaderTimestamp
        {
            HeaderTimestamp( HeaderID ID, uint64_t timestamp ) : m_ID( ID ), m_timestamp( timestamp ) {}

            HeaderID    m_ID;
            uint64_t         m_timestamp;
        };

    public:

        Reflector() = default;

        bool ParseSolution( FileSystem::Path const& slnPath );
        bool Clean();
        bool Build();

    private:

        bool LogError( char const* pErrorFormat, ... ) const;
        bool ParseProject( FileSystem::Path const& prjPath );

        HeaderProcessResult ProcessHeaderFile( FileSystem::Path const& filePath, String& exportMacro );
        uint64_t CalculateHeaderChecksum( FileSystem::Path const& engineIncludePath, FileSystem::Path const& filePath );

        bool UpToDateCheck();
        bool ReflectRegisteredHeaders();
        bool WriteTypeData();

    private:

        FileSystem::Path                    m_reflectionDataPath;
        SolutionInfo                        m_solution;
        ReflectionDatabase                  m_database;

        // Up to data checks
        TVector<HeaderTimestamp>            m_registeredHeaderTimestamps;
    };
}