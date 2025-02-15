#include "ResourceBrowser.h"
#include "ResourceBrowser_DescriptorCreator.h"
#include "EngineTools/Resource/RawFileInspector.h"
#include "EngineTools/Resource/ResourceDescriptor.h"
#include "EngineTools/ThirdParty/pfd/portable-file-dialogs.h"
#include "EngineTools/Core/ToolsContext.h"
#include "System/TypeSystem/TypeRegistry.h"
#include "System/FileSystem/FileSystemUtils.h"
#include "System/Profiling.h"
#include "System/Platform/PlatformHelpers_Win32.h"
#include <eastl/sort.h>
#include "EngineTools/Core/Helpers/CommonDialogs.h"
#include "EngineTools/Resource/ResourceDatabase.h"

//-------------------------------------------------------------------------

namespace EE
{
    class ResourceBrowserTreeItem final : public TreeListViewItem
    {
    public:

        enum class Type
        {
            Directory = 0,
            File,
        };

    public:

        ResourceBrowserTreeItem( char const* pName, FileSystem::Path const& path, ResourcePath const& resourcePath, ResourceTypeID resourceTypeID = ResourceTypeID() )
            : TreeListViewItem()
            , m_nameID( pName )
            , m_path( path )
            , m_resourcePath( resourcePath )
            , m_resourceTypeID( resourceTypeID )
            , m_type( path.IsFilePath() ? Type::File : Type::Directory )
        {
            EE_ASSERT( m_path.IsValid() );
            EE_ASSERT( m_resourcePath.IsValid() );

            // Directories are not allowed to have resource type IDs set
            if ( IsDirectory() )
            {
                EE_ASSERT( !resourceTypeID.IsValid() );
            }
        }

        virtual StringID GetNameID() const { return m_nameID; }
        virtual uint64_t GetUniqueID() const override { return m_resourcePath.GetID(); }
        virtual bool HasContextMenu() const override { return true; }
        virtual bool IsActivatable() const override { return false; }
        virtual bool IsLeaf() const override { return IsFile(); }

        virtual String GetDisplayName() const override
        {
            String displayName;

            if ( IsDirectory() )
            {
                displayName.sprintf( EE_ICON_FOLDER" %s", GetNameID().c_str() );
            }
            else if( IsResourceFile() )
            {
                displayName.sprintf( EE_ICON_FILE_OUTLINE" %s", GetNameID().c_str() );
            }
            else if ( IsRawFile() )
            {
                displayName.sprintf( EE_ICON_FILE_QUESTION_OUTLINE" %s", GetNameID().c_str() );
            }

            return displayName;
        }

        // File Info
        //-------------------------------------------------------------------------

        inline bool IsFile() const { return m_type == Type::File; }
        inline bool IsDirectory() const { return m_type == Type::Directory; }
        inline FileSystem::Path const& GetFilePath() const { return m_path; }
        inline ResourcePath const& GetResourcePath() const { return m_resourcePath; }

        virtual bool IsDragAndDropSource() const override { return IsFile() && IsResourceFile(); }
        virtual void SetDragAndDropPayloadData() const override { ImGui::SetDragDropPayload( "ResourceFile", (void*) m_resourcePath.c_str(), m_resourcePath.GetString().length() ); }

        // Resource Info
        //-------------------------------------------------------------------------

        inline bool IsRawFile() const { EE_ASSERT( IsFile() ); return !m_resourceTypeID.IsValid(); }
        inline bool IsResourceFile() const { EE_ASSERT( IsFile() ); return m_resourceTypeID.IsValid(); }
        inline ResourceID GetResourceID() const { EE_ASSERT( IsResourceFile() ); return ResourceID( m_resourcePath ); }
        inline ResourceTypeID const& GetResourceTypeID() const { EE_ASSERT( IsFile() ); return m_resourceTypeID; }

        template<typename T>
        inline bool IsResourceOfType() const
        {
            static_assert( std::is_base_of<Resource::IResource, T>::value, "T must derive from IResource" );
            return m_resourceTypeID == T::GetStaticResourceTypeID();
        }

    protected:

        StringID                                m_nameID;
        FileSystem::Path                        m_path;
        ResourcePath                            m_resourcePath;
        ResourceTypeID                          m_resourceTypeID;
        Type                                    m_type;
    };
}

//-------------------------------------------------------------------------

namespace EE
{
    ResourceBrowser::ResourceBrowser( ToolsContext& toolsContext )
        : m_toolsContext( toolsContext )
        , m_dataDirectoryPathDepth( m_toolsContext.GetRawResourceDirectory().GetPathDepth() )
    {
        Memory::MemsetZero( m_nameFilterBuffer, 256 * sizeof( char ) );
        m_onDoubleClickEventID = OnItemDoubleClicked().Bind( [this] ( TreeListViewItem* pItem ) { OnBrowserItemDoubleClicked( pItem ); } );
        m_resourceDatabaseUpdateEventBindingID = toolsContext.m_pResourceDatabase->OnDatabaseUpdated().Bind( [this] () { RebuildBrowserTree(); } );

        CreateDescriptorCategoryTree();

        // Refresh visual state
        RebuildBrowserTree();
        UpdateVisibility();
    }

    ResourceBrowser::~ResourceBrowser()
    {
        OnItemDoubleClicked().Unbind( m_onDoubleClickEventID );
        m_toolsContext.m_pResourceDatabase->OnDatabaseUpdated().Unbind( m_resourceDatabaseUpdateEventBindingID );

        EE::Delete( m_pResourceDescriptorCreator );
        EE::Delete( m_pRawResourceInspector );
    }

    //-------------------------------------------------------------------------

    void ResourceBrowser::CreateDescriptorCategoryTree()
    {
        TypeSystem::TypeRegistry const* pTypeRegistry = m_toolsContext.m_pTypeRegistry;
        TVector<TypeSystem::TypeInfo const*> descriptorTypeInfos = pTypeRegistry->GetAllDerivedTypes( Resource::ResourceDescriptor::GetStaticTypeID(), false, false );

        for ( auto pTypeInfo : descriptorTypeInfos )
        {
            auto pRD = (Resource::ResourceDescriptor const*) pTypeInfo->GetDefaultInstance();
            if ( !pRD->IsUserCreateableDescriptor() )
            {
                continue;
            }

            //-------------------------------------------------------------------------

            String category = pTypeInfo->m_ID.c_str();
            StringUtils::ReplaceAllOccurrencesInPlace( category, "::", "/" );
            StringUtils::ReplaceAllOccurrencesInPlace( category, "EE/", "" );
            size_t const foundIdx = category.find_last_of( '/' );
            EE_ASSERT( foundIdx != String::npos );
            category = category.substr( 0, foundIdx );

            auto pResourceInfo = pTypeRegistry->GetResourceInfoForResourceType( pRD->GetCompiledResourceTypeID() );
            m_categorizedDescriptorTypes.AddItem( category, pResourceInfo->m_friendlyName.c_str(), pTypeInfo );
        }
    }

    //-------------------------------------------------------------------------

    bool ResourceBrowser::Draw( UpdateContext const& context )
    {
        bool isOpen = true;
        if ( ImGui::Begin( GetWindowName(), &isOpen) )
        {
            DrawCreationControls( context );
            DrawFilterOptions( context );
            TreeListView::Draw();
        }
        ImGui::End();

        DrawDialogs();

        //-------------------------------------------------------------------------

        if ( m_pResourceDescriptorCreator != nullptr )
        {
            if ( !m_pResourceDescriptorCreator->Draw() )
            {
                EE::Delete( m_pResourceDescriptorCreator );
            }
        }

        if ( m_pRawResourceInspector != nullptr )
        {
            if ( !m_pRawResourceInspector->DrawDialog() )
            {
                EE::Delete( m_pRawResourceInspector );
            }
        }

        //-------------------------------------------------------------------------

        return isOpen;
    }

    void ResourceBrowser::RebuildTreeUserFunction()
    {
        if ( !FileSystem::GetDirectoryContents( m_toolsContext.GetRawResourceDirectory(), m_foundPaths, FileSystem::DirectoryReaderOutput::All, FileSystem::DirectoryReaderMode::Expand) )
        {
            EE_HALT();
        }

        //-------------------------------------------------------------------------

        for ( auto const& path : m_foundPaths )
        {
            auto& parentItem = FindOrCreateParentForItem( path );
            if ( path.IsFilePath() )
            {
                // Check if this is a registered resource
                ResourceTypeID resourceTypeID;
                auto const extension = path.GetLowercaseExtensionAsString();
                if ( extension.length() <= 4 )
                {
                    resourceTypeID = ResourceTypeID( extension.c_str() );
                    if ( !m_toolsContext.m_pTypeRegistry->IsRegisteredResourceType( resourceTypeID ) )
                    {
                        resourceTypeID = ResourceTypeID();
                    }
                }

                // Create file item
                parentItem.CreateChild<ResourceBrowserTreeItem>( path.GetFilename().c_str(), path, ResourcePath::FromFileSystemPath( m_toolsContext.GetRawResourceDirectory(), path ), resourceTypeID );
            }
        }

        UpdateVisibility();
    }

    //-------------------------------------------------------------------------

    void ResourceBrowser::UpdateVisibility()
    {
        auto VisibilityFunc = [this] ( TreeListViewItem const* pItem )
        {
            bool isVisible = true;

            // Type filter
            //-------------------------------------------------------------------------

            auto pDataFileItem = static_cast<ResourceBrowserTreeItem const*>( pItem );
            if ( pDataFileItem->IsFile() )
            {
                if ( pDataFileItem->IsRawFile() )
                {
                    isVisible = m_showRawFiles;
                }
                else // Resource file
                {
                    auto const& resourceTypeID = pDataFileItem->GetResourceTypeID();
                    isVisible = m_typeFilter.empty() || VectorContains( m_typeFilter, resourceTypeID );
                }
            }

            // Text filter
            //-------------------------------------------------------------------------

            if ( isVisible && m_nameFilterBuffer[0] != 0 )
            {
                String lowercaseLabel = pItem->GetDisplayName();
                lowercaseLabel.make_lower();

                char tempBuffer[256];
                strcpy( tempBuffer, m_nameFilterBuffer );

                char* token = strtok( tempBuffer, " " );
                while ( token )
                {
                    if ( lowercaseLabel.find( token ) == String::npos )
                    {
                        isVisible = false;
                        break;
                    }

                    token = strtok( NULL, " " );
                }
            }

            //-------------------------------------------------------------------------

            return isVisible;
        };

        //-------------------------------------------------------------------------

        UpdateItemVisibility( VisibilityFunc );
    }

    void ResourceBrowser::DrawCreationControls( UpdateContext const& context )
    {
        ImGuiX::ScopedFont const sf( ImGuiX::Font::SmallBold );
        float const availableWidth = ImGui::GetContentRegionAvail().x;
        float const buttonWidth = ( availableWidth - 4.0f ) / 2.0f;

        if ( ImGuiX::ColoredButton( Colors::Green, Colors::White, EE_ICON_PLUS" CREATE", ImVec2( buttonWidth, 0 ) ) )
        {
            ImGui::OpenPopup( "CreateNewDescriptor" );
        }
        ImGuiX::ItemTooltip( "Create a new resource descriptor and fill out the settings by hand! This is how you create resources that dont have a source file." );

        if ( ImGui::BeginPopup( "CreateNewDescriptor" ) )
        {
            DrawDescriptorMenuCategory( m_toolsContext.GetRawResourceDirectory(), m_categorizedDescriptorTypes.GetRootCategory() );
            ImGui::EndPopup();
        }
        ImGuiX::ItemTooltip( "Create a resource descriptor based on a source file. This will pop up a helper window to help you with the creation." );

        //-------------------------------------------------------------------------

        ImGui::SameLine( 0, 4 );
        if ( ImGuiX::ColoredButton( Colors::OrangeRed, Colors::White, EE_ICON_FILE_IMPORT" IMPORT", ImVec2( buttonWidth, 0 ) ) )
        {
            auto const selectedFile = pfd::open_file( "Load Map", m_toolsContext.GetRawResourceDirectory().c_str() ).result();
            if ( !selectedFile.empty() )
            {
                FileSystem::Path selectedFilePath( selectedFile[0].data() );
                if ( !selectedFilePath.IsUnderDirectory( m_toolsContext.GetRawResourceDirectory() ) )
                {
                    pfd::message( "Import Error", "File to import must be within the raw resource folder!", pfd::choice::ok, pfd::icon::error );
                }
                else
                {
                    if ( Resource::RawFileInspectorFactory::CanCreateInspector( selectedFilePath ) )
                    {
                        m_pRawResourceInspector = Resource::RawFileInspectorFactory::TryCreateInspector( &m_toolsContext, selectedFilePath );
                    }
                    else
                    {
                        pfd::message( "Import Error", "File type is not importable!", pfd::choice::ok, pfd::icon::error );
                    }
                }
            }
        }
    }

    void ResourceBrowser::DrawFilterOptions( UpdateContext const& context )
    {
        EE_PROFILE_FUNCTION();

        constexpr static float const buttonWidth = 26;
        bool shouldUpdateVisibility = false;

        // Text Filter
        //-------------------------------------------------------------------------

        float itemSpacing = ImGui::GetStyle().ItemSpacing.x;

        ImGui::SetNextItemWidth( ImGui::GetContentRegionAvail().x - buttonWidth - itemSpacing );
        if ( ImGui::InputText( "##Filter", m_nameFilterBuffer, 256 ) )
        {
            // Convert buffer to lower case
            int32_t i = 0;
            while ( i < 256 && m_nameFilterBuffer[i] != 0 )
            {
                m_nameFilterBuffer[i] = eastl::CharToLower( m_nameFilterBuffer[i] );
                i++;
            }

            shouldUpdateVisibility = true;

            auto const SetExpansion = []( TreeListViewItem* pItem )
            {
                if ( pItem->IsVisible() )
                {
                    pItem->SetExpanded( true );
                }
            };

            ForEachItem( SetExpansion );
        }

        ImGui::SameLine();
        if ( ImGui::Button( EE_ICON_CLOSE_CIRCLE "##Clear Filter", ImVec2( buttonWidth, 0 ) ) )
        {
            m_nameFilterBuffer[0] = 0;
            shouldUpdateVisibility = true;
        }

        // Type Filter + Controls
        //-------------------------------------------------------------------------

        float const availableWidth = ImGui::GetContentRegionAvail().x;
        float const filterWidth = availableWidth - ( buttonWidth * 2 ) - ( ImGui::GetStyle().ItemSpacing.x * 2 );
        shouldUpdateVisibility |= DrawResourceTypeFilterMenu( filterWidth );

        ImGui::SameLine();
        if ( ImGui::Button( EE_ICON_PLUS "##Expand All", ImVec2( buttonWidth, 0 ) ) )
        {
            ForEachItem( [] ( TreeListViewItem* pItem ) { pItem->SetExpanded( true ); } );
        }
        ImGuiX::ItemTooltip( "Expand All" );

        ImGui::SameLine();
        if ( ImGui::Button( EE_ICON_MINUS "##Collapse ALL", ImVec2( buttonWidth, 0 ) ) )
        {
            ForEachItem( [] ( TreeListViewItem* pItem ) { pItem->SetExpanded( false ); } );
        }
        ImGuiX::ItemTooltip( "Collapse All" );

        //-------------------------------------------------------------------------

        if ( shouldUpdateVisibility )
        {
            UpdateVisibility();
        }
    }

    bool ResourceBrowser::DrawResourceTypeFilterMenu( float width )
    {
        bool requiresVisibilityUpdate = false;

        ImGui::SetNextItemWidth( width );
        if ( ImGui::BeginCombo( "##ResourceTypeFilters", "Resource Filters", ImGuiComboFlags_HeightLarge ) )
        {
            if ( ImGui::Checkbox( "Raw Files", &m_showRawFiles ) )
            {
                requiresVisibilityUpdate = true;
            }

            ImGui::Separator();

            for ( auto const& resourceInfo : m_toolsContext.m_pTypeRegistry->GetRegisteredResourceTypes() )
            {
                bool isChecked = VectorContains( m_typeFilter, resourceInfo.second.m_resourceTypeID );
                if ( ImGui::Checkbox( resourceInfo.second.m_friendlyName.c_str(), &isChecked ) )
                {
                    if ( isChecked )
                    {
                        m_typeFilter.emplace_back( resourceInfo.second.m_resourceTypeID );
                    }
                    else
                    {
                        m_typeFilter.erase_first_unsorted( resourceInfo.second.m_resourceTypeID );
                    }

                    requiresVisibilityUpdate = true;
                }
            }

            ImGui::EndCombo();
        }

        //-------------------------------------------------------------------------

        return requiresVisibilityUpdate;
    }

    //-------------------------------------------------------------------------

    TreeListViewItem& ResourceBrowser::FindOrCreateParentForItem( FileSystem::Path const& path )
    {
        //EE_ASSERT( path.IsFilePath() );

        TreeListViewItem* pCurrentItem = &m_rootItem;
        FileSystem::Path directoryPath = m_toolsContext.GetRawResourceDirectory();
        TInlineVector<String, 10> splitPath = path.Split();

        //-------------------------------------------------------------------------

        int32_t const pathDepth = (int32_t) splitPath.size();
        for ( int32_t i = m_dataDirectoryPathDepth + 1; i < pathDepth; i++ )
        {
            directoryPath.Append( splitPath[i] );

            StringID const ID( splitPath[i] );
            auto searchPredicate = [&ID] ( TreeListViewItem const* pItem ) { return pItem->GetNameID() == ID; };

            auto pFoundChildItem = pCurrentItem->FindChild( searchPredicate );
            if ( pFoundChildItem == nullptr )
            {
                auto pItem = pCurrentItem->CreateChild<ResourceBrowserTreeItem>( splitPath[i].c_str(), directoryPath, ResourcePath::FromFileSystemPath( m_toolsContext.GetRawResourceDirectory(), directoryPath ) );
                pCurrentItem = pItem;
            }
            else
            {
                pCurrentItem = pFoundChildItem;
            }
        }

        //-------------------------------------------------------------------------

        return *pCurrentItem;
    }

    void ResourceBrowser::DrawItemContextMenu( TVector<TreeListViewItem*> const& selectedItemsWithContextMenus )
    {
        auto pResourceItem = (ResourceBrowserTreeItem*) GetSelection()[0];

        //-------------------------------------------------------------------------

        if ( ImGui::MenuItem( "Open In Explorer" ) )
        {
            Platform::Win32::OpenInExplorer( pResourceItem->GetFilePath() );
        }

        if ( ImGui::MenuItem( "Copy File Path" ) )
        {
            ImGui::SetClipboardText( pResourceItem->GetFilePath().c_str() );
        }

        if ( ImGui::MenuItem( "Copy Resource Path" ) )
        {
            ImGui::SetClipboardText( pResourceItem->GetResourcePath().c_str() );
        }

        // Directory options
        //-------------------------------------------------------------------------

        if ( pResourceItem->GetFilePath().IsDirectoryPath() )
        {
            ImGui::Separator();

            if ( ImGui::BeginMenu( "Create New Descriptor" ) )
            {
                DrawDescriptorMenuCategory( pResourceItem->GetFilePath(), m_categorizedDescriptorTypes.GetRootCategory() );
                ImGui::EndMenu();
            }
        }

        // File options
        //-------------------------------------------------------------------------

        if ( pResourceItem->GetFilePath().IsFilePath() )
        {
            ImGui::Separator();

            if ( ImGui::MenuItem( EE_ICON_ALERT_OCTAGON" Delete" ) )
            {
                m_showDeleteConfirmationDialog = true;
            }
        }
    }

    void ResourceBrowser::DrawDialogs()
    {
        if ( m_showDeleteConfirmationDialog )
        {
            ImGui::OpenPopup( "Delete Resource" );
            m_showDeleteConfirmationDialog = false;
        }

        //-------------------------------------------------------------------------

        ImGui::SetNextWindowSize( ImVec2( 300, 96 ) );
        if ( ImGui::BeginPopupModal( "Delete Resource", nullptr, ImGuiWindowFlags_NoResize ) )
        {
            ImGui::Text( "Are you sure you want to delete this file?" );
            ImGui::Text( "This cannot be undone!" );

            if ( ImGui::Button( "Ok", ImVec2( 143, 0 ) ) )
            {
                auto pResourceItem = (ResourceBrowserTreeItem*) GetSelection()[0];
                FileSystem::Path const fileToDelete = pResourceItem->GetFilePath();
                ClearSelection();
                FileSystem::EraseFile( fileToDelete );
                ImGui::CloseCurrentPopup();
            }

            ImGui::SameLine( 0, 6 );

            if ( ImGui::Button( "Cancel", ImVec2( 143, 0 ) ) )
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::SetItemDefaultFocus();

            ImGui::EndPopup();
        }
    }

    void ResourceBrowser::OnBrowserItemDoubleClicked( TreeListViewItem* pItem )
    {
        auto pResourceFileItem = static_cast<ResourceBrowserTreeItem const*>( pItem );
        if ( pResourceFileItem->IsDirectory() )
        {
            return;
        }

        //-------------------------------------------------------------------------

        if ( pResourceFileItem->IsResourceFile() )
        {
            m_toolsContext.TryOpenResource( pResourceFileItem->GetResourceID() );
        }
        else // Try create file inspector
        {
            if ( Resource::RawFileInspectorFactory::CanCreateInspector( pResourceFileItem->GetFilePath() ) )
            {
                m_pRawResourceInspector = Resource::RawFileInspectorFactory::TryCreateInspector( &m_toolsContext, pResourceFileItem->GetFilePath() );
            }
        }
    }

    //-------------------------------------------------------------------------

    void ResourceBrowser::DrawDescriptorMenuCategory( FileSystem::Path const& path, Category<TypeSystem::TypeInfo const*> const& category )
    {
        auto DrawItems = [this, &path, &category] ()
        {
            for ( auto const& childCategory : category.m_childCategories )
            {
                DrawDescriptorMenuCategory( path, childCategory );
            }

            for ( auto const& item : category.m_items )
            {
                if ( ImGui::MenuItem( item.m_name.c_str() ) )
                {
                    m_pResourceDescriptorCreator = EE::New<ResourceDescriptorCreator>( &m_toolsContext, item.m_data->m_ID, path );
                }
            }
        };

        //-------------------------------------------------------------------------

        if ( category.m_depth == -1 )
        {
            DrawItems();
        }
        else
        {
            if ( ImGui::BeginMenu( category.m_name.c_str() ) )
            {
                DrawItems();
                ImGui::EndMenu();
            }
        }
    }
}