#pragma once

#include "EngineTools/Core/PropertyGrid/PropertyGrid.h"
#include "System/FileSystem/FileSystemPath.h"
#include "System/Resource/ResourceID.h"

namespace EE {}

//-------------------------------------------------------------------------

namespace EE
{
    class ToolsContext;
    namespace Resource { struct ResourceDescriptor; }

    //-------------------------------------------------------------------------

    class ResourceDescriptorCreator
    {
        constexpr static char const* s_title = "Create Descriptor";

    public:

        ResourceDescriptorCreator( ToolsContext* toolsContext, TypeSystem::TypeID const descriptorTypeID, FileSystem::Path const& startingDir );
        virtual ~ResourceDescriptorCreator();

        // Returns true if we should keep drawing this widget. Returns false when we are done!
        bool Draw();

    private:

        void SaveDescriptor();

    protected:

        ToolsContext*                               m_pToolsContext = nullptr;
        Resource::ResourceDescriptor*               m_pDescriptor = nullptr;
        PropertyGrid                                m_propertyGrid;
        FileSystem::Path                            m_startingPath;
    };
}