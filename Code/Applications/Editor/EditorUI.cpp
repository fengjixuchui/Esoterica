#include "EditorUI.h"
#include "RenderingSystem.h"
#include "EngineTools/Entity/Workspaces/Workspace_MapEditor.h"
#include "EngineTools/Entity/Workspaces/Workspace_GamePreviewer.h"
#include "EngineTools/ThirdParty/pfd/portable-file-dialogs.h"
#include "Engine/Physics/Debug/DebugView_Physics.h"
#include "Engine/ToolsUI/OrientationGuide.h"
#include "Engine/Entity/EntityWorld.h"
#include "Engine/DebugViews/DebugView_Resource.h"
#include "Engine/Entity/EntityWorldManager.h"
#include "Engine/Entity/EntityWorldUpdateContext.h"
#include "Engine/Render/DebugViews/DebugView_Render.h"
#include "System/Resource/ResourceSystem.h"
#include "System/Resource/ResourceSettings.h"
#include "System/TypeSystem/TypeRegistry.h"

//-------------------------------------------------------------------------

namespace EE
{
    EditorUI::~EditorUI()
    {
        EE_ASSERT( m_workspaces.empty() );
        EE_ASSERT( m_pMapEditor == nullptr );
        EE_ASSERT( m_pGamePreviewer == nullptr );

        EE_ASSERT( m_pResourceBrowser == nullptr );
        EE_ASSERT( m_pRenderingSystem == nullptr );
        EE_ASSERT( m_pWorldManager == nullptr );
    }

    void EditorUI::SetStartupMap( ResourceID const& mapID )
    {
        EE_ASSERT( mapID.IsValid() );

        if ( mapID.GetResourceTypeID() == EntityModel::SerializedEntityMap::GetStaticResourceTypeID() )
        {
            m_startupMapResourceID = mapID;
        }
        else
        {
            EE_LOG_ERROR( "Editor", "Invalid startup map resource supplied: %s", m_startupMapResourceID.c_str() );
        }
    }

    void EditorUI::Initialize( UpdateContext const& context )
    {
        // Systems
        //-------------------------------------------------------------------------

        m_pTypeRegistry = context.GetSystem<TypeSystem::TypeRegistry>();
        m_pWorldManager = context.GetSystem<EntityWorldManager>();
        m_pRenderingSystem = context.GetSystem<Render::RenderingSystem>();

        // Resources
        //-------------------------------------------------------------------------

        auto pResourceSystem = context.GetSystem<Resource::ResourceSystem>();
        m_resourceDB.Initialize( m_pTypeRegistry, pResourceSystem->GetSettings().m_rawResourcePath, pResourceSystem->GetSettings().m_compiledResourcePath );
        m_resourceDeletedEventID = m_resourceDB.OnResourceDeleted().Bind( [this] ( ResourceID const& resourceID ) { OnResourceDeleted( resourceID ); } );
        m_pResourceDatabase = &m_resourceDB;
        m_pResourceBrowser = EE::New<ResourceBrowser>( *this );

        // Map Editor
        //-------------------------------------------------------------------------

        // Destroy the default created game world
        m_pWorldManager->DestroyWorld( m_pWorldManager->GetWorlds()[0] );

        // Create a new editor world for the map editor workspace
        auto pMapEditorWorld = m_pWorldManager->CreateWorld( EntityWorldType::Tools );
        m_pRenderingSystem->CreateCustomRenderTargetForViewport( pMapEditorWorld->GetViewport(), true );

        // Create the map editor workspace
        m_pMapEditor = EE::New<EntityModel::EntityMapEditor>( this, pMapEditorWorld );
        m_pMapEditor->Initialize( context );
        m_workspaces.emplace_back( m_pMapEditor );

        // Create bindings to start/stop game preview
        m_gamePreviewStartedEventBindingID = m_pMapEditor->OnGamePreviewStartRequested().Bind( [this] ( UpdateContext const& context ) { CreateGamePreviewWorkspace( context ); } );
        m_gamePreviewStoppedEventBindingID = m_pMapEditor->OnGamePreviewStopRequested().Bind( [this] ( UpdateContext const& context ) { DestroyGamePreviewWorkspace( context ); } );

        // Load startup map
        if ( m_startupMapResourceID.IsValid() )
        {
            EE_ASSERT( m_startupMapResourceID.GetResourceTypeID() == EntityModel::SerializedEntityMap::GetStaticResourceTypeID() );
            m_pMapEditor->LoadMap( m_startupMapResourceID );
        }
    }

    void EditorUI::Shutdown( UpdateContext const& context )
    {
        // Map Editor
        //-------------------------------------------------------------------------

        EE_ASSERT( m_pMapEditor != nullptr );
        m_pMapEditor->OnGamePreviewStartRequested().Unbind( m_gamePreviewStartedEventBindingID );
        m_pMapEditor->OnGamePreviewStopRequested().Unbind( m_gamePreviewStoppedEventBindingID );
        m_pMapEditor = nullptr;
        m_pGamePreviewer = nullptr;

        // Workspaces
        //-------------------------------------------------------------------------

        while ( !m_workspaces.empty() )
        {
            DestroyWorkspace( context, m_workspaces[0] );
        }

        m_workspaces.clear();

        // Resources
        //-------------------------------------------------------------------------

        EE::Delete( m_pResourceBrowser );
        m_pResourceDatabase = nullptr;
        m_resourceDB.OnResourceDeleted().Unbind( m_resourceDeletedEventID );
        m_resourceDB.Shutdown();

        // Systems
        //-------------------------------------------------------------------------

        m_pWorldManager = nullptr;
        m_pRenderingSystem = nullptr;
        m_pTypeRegistry = nullptr;
    }

    void EditorUI::TryOpenResource( ResourceID const& resourceID ) const
    {
        if ( resourceID.IsValid() )
        {
            const_cast<EditorUI*>( this )->QueueCreateWorkspace( resourceID );
        }
    }

    //-------------------------------------------------------------------------
    // Update
    //-------------------------------------------------------------------------

    void EditorUI::StartFrame( UpdateContext const& context )
    {
        UpdateStage const updateStage = context.GetUpdateStage();
        EE_ASSERT( updateStage == UpdateStage::FrameStart );

        //-------------------------------------------------------------------------
        // Resource Systems
        //-------------------------------------------------------------------------

        m_resourceDB.Update();

        //-------------------------------------------------------------------------
        // Workspace Management
        //-------------------------------------------------------------------------

        // Destroy all required workspaces
        // We needed to defer this to the start of the update since we may have references resources that we might unload (i.e. textures)
        for ( auto pWorkspaceToDestroy : m_workspaceDestructionRequests )
        {
            DestroyWorkspace( context, pWorkspaceToDestroy );
        }
        m_workspaceDestructionRequests.clear();

        // Create all workspaces
        for ( auto const& resourceID : m_workspaceCreationRequests )
        {
            TryCreateWorkspace( context, resourceID );
        }
        m_workspaceCreationRequests.clear();

        //-------------------------------------------------------------------------
        // Main Menu
        //-------------------------------------------------------------------------

        if ( ImGui::BeginMainMenuBar() )
        {
            DrawMainMenu( context );
            ImGui::EndMainMenuBar();
        }

        //-------------------------------------------------------------------------
        // Create main dock window
        //-------------------------------------------------------------------------

        m_editorWindowClass.ClassId = ImGui::GetID( "EditorWindowClass" );
        m_editorWindowClass.DockingAllowUnclassed = false;

        ImGuiID const dockspaceID = ImGui::GetID( "EditorDockSpace" );

        ImGuiWindowFlags const windowFlags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

        ImGuiViewport const* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos( viewport->WorkPos );
        ImGui::SetNextWindowSize( viewport->WorkSize );
        ImGui::SetNextWindowViewport( viewport->ID );

        ImGui::PushStyleVar( ImGuiStyleVar_WindowRounding, 0.0f );
        ImGui::PushStyleVar( ImGuiStyleVar_WindowBorderSize, 0.0f );
        ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 0.0f, 0.0f ) );
        ImGui::Begin( "EditorDockSpaceWindow", nullptr, windowFlags );
        ImGui::PopStyleVar( 3 );
        {
            if ( !ImGui::DockBuilderGetNode( dockspaceID ) )
            {
                ImGui::DockBuilderAddNode( dockspaceID, ImGuiDockNodeFlags_DockSpace );
                ImGui::DockBuilderSetNodeSize( dockspaceID, ImGui::GetContentRegionAvail() );
                ImGuiID leftDockID = 0, rightDockID = 0;
                ImGui::DockBuilderSplitNode( dockspaceID, ImGuiDir_Left, 0.25f, &leftDockID, &rightDockID );
                ImGui::DockBuilderFinish( dockspaceID );

                ImGui::DockBuilderDockWindow( m_pResourceBrowser->GetWindowName(), leftDockID );
                ImGui::DockBuilderDockWindow( m_pMapEditor->GetWorkspaceWindowID(), rightDockID );
            }

            // Create the actual dock space
            ImGui::PushStyleVar( ImGuiStyleVar_TabRounding, 0 );
            ImGui::DockSpace( dockspaceID, viewport->WorkSize, ImGuiDockNodeFlags_None, &m_editorWindowClass );
            ImGui::PopStyleVar( 1 );
        }
        ImGui::End();

        //-------------------------------------------------------------------------
        // Draw editor windows
        //-------------------------------------------------------------------------

        if ( m_isResourceBrowserWindowOpen )
        {
            ImGui::SetNextWindowClass( &m_editorWindowClass );
            m_isResourceBrowserWindowOpen = m_pResourceBrowser->Draw( context );
        }

        if ( m_isResourceOverviewWindowOpen )
        {
            ImGui::SetNextWindowClass( &m_editorWindowClass );
            auto pResourceSystem = context.GetSystem<Resource::ResourceSystem>();
            Resource::ResourceDebugView::DrawOverviewWindow( pResourceSystem, &m_isResourceOverviewWindowOpen );
        }

        if ( m_isResourceLogWindowOpen )
        {
            ImGui::SetNextWindowClass( &m_editorWindowClass );
            auto pResourceSystem = context.GetSystem<Resource::ResourceSystem>();
            Resource::ResourceDebugView::DrawLogWindow( pResourceSystem, &m_isResourceLogWindowOpen );
        }

        if ( m_isSystemLogWindowOpen )
        {
            ImGui::SetNextWindowClass( &m_editorWindowClass );
            m_isSystemLogWindowOpen = m_systemLogView.Draw( context );
        }

        if ( m_isPhysicsMaterialDatabaseWindowOpen )
        {
            ImGui::SetNextWindowClass( &m_editorWindowClass );
            m_isPhysicsMaterialDatabaseWindowOpen = Physics::PhysicsDebugView::DrawMaterialDatabaseView( context );
        }

        if ( m_isImguiDemoWindowOpen )
        {
            ImGui::ShowDemoWindow( &m_isImguiDemoWindowOpen );
        }

        if ( m_isUITestWindowOpen )
        {
            DrawUITestWindow();
        }

        //-------------------------------------------------------------------------
        // Draw open workspaces
        //-------------------------------------------------------------------------

        // Reset mouse state, this is updated via the workspaces
        Workspace* pWorkspaceToClose = nullptr;

        // Draw all workspaces
        for ( auto pWorkspace : m_workspaces )
        {
            if ( pWorkspace == m_pGamePreviewer )
            {
                continue;
            }

            ImGui::SetNextWindowClass( &m_editorWindowClass );
            if ( !DrawWorkspaceWindow( context, pWorkspace ) )
            {
                pWorkspaceToClose = pWorkspace;
            }
        }

        // Did we get a close request?
        if ( pWorkspaceToClose != nullptr )
        {
            // We need to defer this to the start of the update since we may have references resources that we might unload (i.e. textures)
            QueueDestroyWorkspace( pWorkspaceToClose );
        }

        //-------------------------------------------------------------------------
        // Handle Warnings/Errors
        //-------------------------------------------------------------------------

        auto const unhandledWarningsAndErrors = Log::GetUnhandledWarningsAndErrors();
        if ( !unhandledWarningsAndErrors.empty() )
        {
            m_isSystemLogWindowOpen = true;
        }
    }

    void EditorUI::EndFrame( UpdateContext const& context )
    {
        // Game previewer needs to be drawn at the end of the frames since then all the game simulation data will be correct and all the debug tools will be accurate
        if ( m_pGamePreviewer != nullptr )
        {
            if ( !DrawWorkspaceWindow( context, m_pGamePreviewer ) )
            {
                QueueDestroyWorkspace( m_pGamePreviewer );
            }
        }
    }

    void EditorUI::Update( UpdateContext const& context )
    {
        for ( auto pWorkspace : m_workspaces )
        {
            EntityWorldUpdateContext updateContext( context, pWorkspace->GetWorld() );
            pWorkspace->PreUpdateWorld( updateContext );
        }
    }

    //-------------------------------------------------------------------------
    // Hot Reload
    //-------------------------------------------------------------------------

    void EditorUI::BeginHotReload( TVector<Resource::ResourceRequesterID> const& usersToBeReloaded, TVector<ResourceID> const& resourcesToBeReloaded )
    {
        for ( auto pWorkspace : m_workspaces )
        {
            pWorkspace->BeginHotReload( usersToBeReloaded, resourcesToBeReloaded );
        }
    }

    void EditorUI::EndHotReload()
    {
        for ( auto pWorkspace : m_workspaces )
        {
            pWorkspace->EndHotReload();
        }
    }

    //-------------------------------------------------------------------------
    // Resource Management
    //-------------------------------------------------------------------------

    void EditorUI::OnResourceDeleted( ResourceID const& resourceID )
    {
        EE_ASSERT( resourceID.IsValid() );

        for ( auto pWorkspace : m_workspaces )
        {
            if ( pWorkspace->HasDependencyOnResource( resourceID ) )
            {
                QueueDestroyWorkspace( pWorkspace );
            }
        }
    }

    //-------------------------------------------------------------------------
    // Workspace Management
    //-------------------------------------------------------------------------

    bool EditorUI::TryCreateWorkspace( UpdateContext const& context, ResourceID const& resourceID )
    {
        ResourceTypeID const resourceTypeID = resourceID.GetResourceTypeID();

        // Don't try to open invalid resource IDs
        if ( !m_resourceDB.DoesResourceExist( resourceID ) )
        {
            return false;
        }

        // Handle maps explicitly
        //-------------------------------------------------------------------------

        if ( resourceTypeID == EntityModel::SerializedEntityMap::GetStaticResourceTypeID() )
        {
            m_pMapEditor->LoadMap( resourceID );
            ImGuiX::MakeTabVisible( m_pMapEditor->GetWorkspaceWindowID() );
            return true;
        }

        // Other resource types
        //-------------------------------------------------------------------------

        // Check if we already have a workspace open for this resource, if so then switch focus to it
        for ( auto pWorkspace : m_workspaces )
        {
            if ( pWorkspace->IsWorkingOnResource( resourceID ) )
            {
                ImGuiX::MakeTabVisible( pWorkspace->GetWorkspaceWindowID() );
                return true;
            }
        }

        // Check if we can create a new workspace
        if ( !ResourceWorkspaceFactory::CanCreateWorkspace( this, resourceID ) )
        {
            return false;
        }

        // Create tools world
        auto pToolsWorld = m_pWorldManager->CreateWorld( EntityWorldType::Tools );
        pToolsWorld->LoadMap( ResourcePath( "data://Editor/EditorMap.map" ) );
        m_pRenderingSystem->CreateCustomRenderTargetForViewport( pToolsWorld->GetViewport() );

        // Create workspace
        auto pCreatedWorkspace = ResourceWorkspaceFactory::CreateWorkspace( this, pToolsWorld, resourceID );
        pCreatedWorkspace->Initialize( context );
        m_workspaces.emplace_back( pCreatedWorkspace );

        return true;
    }

    void EditorUI::QueueCreateWorkspace( ResourceID const& resourceID )
    {
        m_workspaceCreationRequests.emplace_back( resourceID );
    }

    void EditorUI::DestroyWorkspace( UpdateContext const& context, Workspace* pWorkspace )
    {
        EE_ASSERT( m_pMapEditor != pWorkspace );
        EE_ASSERT( pWorkspace != nullptr );

        auto foundWorkspaceIter = eastl::find( m_workspaces.begin(), m_workspaces.end(), pWorkspace );
        EE_ASSERT( foundWorkspaceIter != m_workspaces.end() );

        if ( pWorkspace->IsDirty() )
        {
            auto messageDialog = pfd::message( "Unsaved Changes", "You have unsaved changes!\nDo you wish to save these changes before closing?", pfd::choice::yes_no_cancel );
            switch ( messageDialog.result() )
            {
                case pfd::button::yes:
                {
                    if ( !pWorkspace->Save() )
                    {
                        return;
                    }
                }
                break;

                case pfd::button::cancel:
                {
                    return;
                }
                break;
            }
        }

        //-------------------------------------------------------------------------

        bool const isGamePreviewerWorkspace = m_pGamePreviewer == pWorkspace;

        // Destroy the custom viewport render target
        auto pPreviewWorld = pWorkspace->GetWorld();
        m_pRenderingSystem->DestroyCustomRenderTargetForViewport( pPreviewWorld->GetViewport() );

        // Destroy workspace
        pWorkspace->Shutdown( context );
        EE::Delete( pWorkspace );
        m_workspaces.erase( foundWorkspaceIter );

        // Clear the game previewer workspace ptr if we just destroyed it
        if ( isGamePreviewerWorkspace )
        {
            m_pMapEditor->NotifyGamePreviewEnded();
            m_pGamePreviewer = nullptr;
        }

        // Destroy preview world
        m_pWorldManager->DestroyWorld( pPreviewWorld );
    }

    void EditorUI::QueueDestroyWorkspace( Workspace* pWorkspace )
    {
        EE_ASSERT( m_pMapEditor != pWorkspace );
        m_workspaceDestructionRequests.emplace_back( pWorkspace );
    }

    bool EditorUI::DrawWorkspaceWindow( UpdateContext const& context, Workspace* pWorkspace )
    {
        EE_ASSERT( pWorkspace != nullptr );

        //-------------------------------------------------------------------------
        // Create Workspace Window
        //-------------------------------------------------------------------------
        // This is an empty window that just contains the dockspace for the workspace

        bool isTabOpen = true;
        bool* pIsTabOpen = ( pWorkspace == m_pMapEditor ) ? nullptr : &isTabOpen; // Prevent closing the map-editor workspace

        ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoCollapse;

        if ( pWorkspace->HasWorkspaceToolbar() )
        {
            windowFlags |= ImGuiWindowFlags_MenuBar;
        }

        if ( pWorkspace->IsDirty() )
        {
            windowFlags |= ImGuiWindowFlags_UnsavedDocument;
        }

        ImGui::SetNextWindowSizeConstraints( ImVec2( 128, 128 ), ImVec2( FLT_MAX, FLT_MAX ) );
        ImGui::SetNextWindowSize( ImVec2( 1024, 768 ), ImGuiCond_FirstUseEver );
        ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 0, 0 ) );
        bool const shouldDrawWindowContents = ImGui::Begin( pWorkspace->GetWorkspaceWindowID(), pIsTabOpen, windowFlags );
        bool const isFocused = ImGui::IsWindowFocused( ImGuiFocusedFlags_ChildWindows | ImGuiFocusedFlags_DockHierarchy );
        ImGui::PopStyleVar();

        // Draw Workspace Menu
        //-------------------------------------------------------------------------

        if ( pWorkspace->HasWorkspaceToolbar() )
        {
            if ( ImGui::BeginMenuBar() )
            {
                pWorkspace->DrawWorkspaceToolbar( context );
                ImGui::EndMenuBar();
            }
        }

        // Create dockspace
        //-------------------------------------------------------------------------

        ImGuiID const dockspaceID = ImGui::GetID( pWorkspace->GetDockspaceID() );
        ImGuiWindowClass workspaceWindowClass;
        workspaceWindowClass.ClassId = dockspaceID;
        workspaceWindowClass.DockingAllowUnclassed = false;

        if ( !ImGui::DockBuilderGetNode( dockspaceID ) )
        {
            ImGui::DockBuilderAddNode( dockspaceID, ImGuiDockNodeFlags_DockSpace | ImGuiDockNodeFlags_NoWindowMenuButton | ImGuiDockNodeFlags_NoCloseButton );
            ImGui::DockBuilderSetNodeSize( dockspaceID, ImGui::GetContentRegionAvail() );
            pWorkspace->InitializeDockingLayout( dockspaceID );
            ImGui::DockBuilderFinish( dockspaceID );
        }

        ImGuiDockNodeFlags const dockFlags = shouldDrawWindowContents ? ImGuiDockNodeFlags_None : ImGuiDockNodeFlags_KeepAliveOnly;
        ImGui::DockSpace( dockspaceID, ImGui::GetContentRegionAvail(), dockFlags, &workspaceWindowClass );

        ImGui::End();

        //-------------------------------------------------------------------------
        // Draw workspace contents
        //-------------------------------------------------------------------------

        bool enableCameraUpdate = false;
        auto pWorldManager = context.GetSystem<EntityWorldManager>();
        auto pWorld = pWorkspace->GetWorld();

        if ( shouldDrawWindowContents )
        {
            if ( pWorkspace != m_pMapEditor || m_pGamePreviewer == nullptr )
            {
                pWorld->ResumeUpdates();
            }

            if ( pWorkspace->HasViewportWindow() )
            {
                Workspace::ViewportInfo viewportInfo;
                viewportInfo.m_pViewportRenderTargetTexture = (void*) &m_pRenderingSystem->GetRenderTargetTextureForViewport( pWorld->GetViewport() );
                viewportInfo.m_retrievePickingID = [this, pWorld] ( Int2 const& pixelCoords ) { return m_pRenderingSystem->GetViewportPickingID( pWorld->GetViewport(), pixelCoords ); };
                enableCameraUpdate = pWorkspace->DrawViewport( context, viewportInfo, &workspaceWindowClass );
            }

            pWorkspace->InternalSharedUpdate( context, &workspaceWindowClass, isFocused );
            pWorkspace->Update( context, &workspaceWindowClass, isFocused );
        }
        else // If the workspace window is hidden suspend world updates
        {
            pWorld->SuspendUpdates();
        }

        pWorkspace->SetCameraUpdateEnabled( enableCameraUpdate );

        return isTabOpen;
    }

    void EditorUI::CreateGamePreviewWorkspace( UpdateContext const& context )
    {
        EE_ASSERT( m_pGamePreviewer == nullptr );

        auto pPreviewWorld = m_pWorldManager->CreateWorld( EntityWorldType::Game );
        m_pRenderingSystem->CreateCustomRenderTargetForViewport( pPreviewWorld->GetViewport() );
        m_pGamePreviewer = EE::New<GamePreviewer>( this, pPreviewWorld );
        m_pGamePreviewer->Initialize( context );
        m_pGamePreviewer->LoadMapToPreview( m_pMapEditor->GetLoadedMap() );
        m_workspaces.emplace_back( m_pGamePreviewer );

        m_pMapEditor->NotifyGamePreviewStarted();
    }

    void EditorUI::DestroyGamePreviewWorkspace( UpdateContext const& context )
    {
        EE_ASSERT( m_pGamePreviewer != nullptr );
        QueueDestroyWorkspace( m_pGamePreviewer );
    }

    //-------------------------------------------------------------------------
    // Misc
    //-------------------------------------------------------------------------

    void EditorUI::DrawUITestWindow()
    {
        if ( ImGui::Begin( "UI Test", &m_isUITestWindowOpen ) )
        {
            {
                ImGuiX::ScopedFont sf( ImGuiX::Font::Tiny );
                ImGui::Text( EE_ICON_FILE_CHECK"This is a test - Tiny" );
            }
            {
                ImGuiX::ScopedFont sf( ImGuiX::Font::TinyBold );
                ImGui::Text( EE_ICON_ALERT"This is a test - Tiny Bold" );
            }
            {
                ImGuiX::ScopedFont sf( ImGuiX::Font::Small );
                ImGui::Text( EE_ICON_FILE_CHECK"This is a test - Small" );
            }
            {
                ImGuiX::ScopedFont sf( ImGuiX::Font::SmallBold );
                ImGui::Text( EE_ICON_ALERT"This is a test - Small Bold" );
            }
            {
                ImGuiX::ScopedFont sf( ImGuiX::Font::Medium );
                ImGui::Text( EE_ICON_FILE_CHECK"This is a test - Medium" );
            }
            {
                ImGuiX::ScopedFont sf( ImGuiX::Font::MediumBold );
                ImGui::Text( EE_ICON_ALERT"This is a test - Medium Bold" );
            }
            {
                ImGuiX::ScopedFont sf( ImGuiX::Font::Large );
                ImGui::Text( EE_ICON_FILE_CHECK"This is a test - Large" );
            }
            {
                ImGuiX::ScopedFont sf( ImGuiX::Font::LargeBold );
                ImGui::Text( EE_ICON_CCTV_OFF"This is a test - Large Bold" );
            }
            {
                ImGuiX::ScopedFont sf( ImGuiX::Font::Huge );
                ImGui::Text( EE_ICON_FILE_CHECK"This is a test - Huge" );
            }
            {
                ImGuiX::ScopedFont sf( ImGuiX::Font::HugeBold );
                ImGui::Text( EE_ICON_FILE_CHECK"This is a test - Huge Bold" );
            }

            //-------------------------------------------------------------------------

            ImGui::NewLine();

            //-------------------------------------------------------------------------

            {
                ImGuiX::ScopedFont sf( ImGuiX::Font::Small );
                ImGuiX::ColoredButton( Colors::Green, Colors::White, EE_ICON_PLUS"ADD" );
            }
            {
                ImGuiX::ScopedFont sf( ImGuiX::Font::SmallBold );
                ImGuiX::ColoredButton( Colors::Green, Colors::White, EE_ICON_PLUS"ADD" );
            }
            {
                ImGuiX::ScopedFont sf( ImGuiX::Font::Medium );
                ImGuiX::ColoredButton( Colors::Green, Colors::White, EE_ICON_PLUS"ADD" );
            }
            {
                ImGuiX::ScopedFont sf( ImGuiX::Font::MediumBold );
                ImGuiX::ColoredButton( Colors::Green, Colors::White, EE_ICON_PLUS"ADD" );
            }
            {
                ImGuiX::ScopedFont sf( ImGuiX::Font::Large );
                ImGuiX::ColoredButton( Colors::Green, Colors::White, EE_ICON_PLUS"ADD" );
            }
            {
                ImGuiX::ScopedFont sf( ImGuiX::Font::LargeBold );
                ImGuiX::ColoredButton( Colors::Green, Colors::White, EE_ICON_PLUS"ADD" );
            }
            {
                ImGuiX::ScopedFont sf( ImGuiX::Font::Huge );
                ImGuiX::ColoredButton( Colors::Green, Colors::White, EE_ICON_PLUS"ADD" );
            }
            {
                ImGuiX::ScopedFont sf( ImGuiX::Font::HugeBold );
                ImGuiX::ColoredButton( Colors::Green, Colors::White, EE_ICON_PLUS"ADD" );
            }

            //-------------------------------------------------------------------------

            ImGui::NewLine();

            //-------------------------------------------------------------------------

            {
                ImGuiX::ScopedFont sf( ImGuiX::Font::Small );
                ImGui::Button( EE_ICON_HAIR_DRYER );
                ImGui::SameLine();
                ImGui::Button( EE_ICON_Z_WAVE );
                ImGui::SameLine();
                ImGui::Button( EE_ICON_KANGAROO );
                ImGui::SameLine();
                ImGui::Button( EE_ICON_YIN_YANG );
            }

            {
                ImGuiX::ScopedFont sf( ImGuiX::Font::Medium );
                ImGui::Button( EE_ICON_HAIR_DRYER );
                ImGui::SameLine();
                ImGui::Button( EE_ICON_Z_WAVE );
                ImGui::SameLine();
                ImGui::Button( EE_ICON_KANGAROO );
                ImGui::SameLine();
                ImGui::Button( EE_ICON_YIN_YANG );
            }

            {
                ImGuiX::ScopedFont sf( ImGuiX::Font::Large );
                ImGui::Button( EE_ICON_HAIR_DRYER );
                ImGui::SameLine();
                ImGui::Button( EE_ICON_Z_WAVE );
                ImGui::SameLine();
                ImGui::Button( EE_ICON_KANGAROO );
                ImGui::SameLine();
                ImGui::Button( EE_ICON_YIN_YANG );
            }

            {
                ImGuiX::ScopedFont sf( ImGuiX::Font::Huge );
                ImGui::Button( EE_ICON_HAIR_DRYER );
                ImGui::SameLine();
                ImGui::Button( EE_ICON_Z_WAVE );
                ImGui::SameLine();
                ImGui::Button( EE_ICON_KANGAROO );
                ImGui::SameLine();
                ImGui::Button( EE_ICON_YIN_YANG );
            }

            //-------------------------------------------------------------------------

            ImGuiX::IconButton( EE_ICON_KANGAROO, "Test", Colors::PaleGreen, ImVec2( 100, 0 ) );

            ImGuiX::IconButton( EE_ICON_HOME, "Home", Colors::RoyalBlue, ImVec2( 100, 0 ) );

            ImGuiX::IconButton( EE_ICON_MOVIE_PLAY, "Play", Colors::LightPink, ImVec2( 100, 0 ) );

            ImGuiX::ColoredIconButton( Colors::Green, Colors::White, Colors::Yellow, EE_ICON_KANGAROO, "Test", ImVec2( 100, 0 ) );

            ImGuiX::FlatIconButton( EE_ICON_HOME, "Home", Colors::RoyalBlue, ImVec2( 100, 0 ) );

        }
        ImGui::End();
    }

    void EditorUI::DrawMainMenu( UpdateContext const& context )
    {
        ImVec2 const menuDimensions = ImGui::GetContentRegionMax();

        //-------------------------------------------------------------------------
        // Engine
        //-------------------------------------------------------------------------

        if ( ImGui::BeginMenu( "Resource" ) )
        {
            ImGui::MenuItem( "Resource Browser", nullptr, &m_isResourceBrowserWindowOpen );
            ImGui::MenuItem( "Resource System Overview", nullptr, &m_isResourceOverviewWindowOpen );
            ImGui::MenuItem( "Resource Log", nullptr, &m_isResourceLogWindowOpen );
            ImGui::EndMenu();
        }

        if ( ImGui::BeginMenu( "Physics" ) )
        {
            ImGui::MenuItem( "Physics Material DB", nullptr, &m_isPhysicsMaterialDatabaseWindowOpen );
            ImGui::EndMenu();
        }

        if ( ImGui::BeginMenu( "System" ) )
        {
            ImGui::MenuItem( "System Log", nullptr, &m_isSystemLogWindowOpen );

            ImGui::Separator();

            ImGui::MenuItem( "Imgui UI Test Window", nullptr, &m_isUITestWindowOpen );
            ImGui::MenuItem( "Imgui Demo Window", nullptr, &m_isImguiDemoWindowOpen );

            ImGui::EndMenu();
        }

        //-------------------------------------------------------------------------
        // Draw Frame Limiter and Performance Stats
        //-------------------------------------------------------------------------

        float const currentFPS = 1.0f / context.GetDeltaTime();
        float const allocatedMemory = Memory::GetTotalAllocatedMemory() / 1024.0f / 1024.0f;

        TInlineString<100> const perfStats( TInlineString<100>::CtorSprintf(), "FPS: %3.0f", currentFPS );
        TInlineString<100> const memStats( TInlineString<100>::CtorSprintf(), "MEM: %.2fMB", allocatedMemory );

        float const itemSpacing = ImGui::GetStyle().ItemSpacing.x;
        float const frameLimiterSize = 30;
        float const perfStatsSize = 64;
        float const memStatsSize = ImGui::CalcTextSize( memStats.c_str() ).x;

        float const memStatsOffset = menuDimensions.x - ( itemSpacing * 2 ) - memStatsSize;
        float const perfStatsOffset = memStatsOffset - perfStatsSize;
        float const frameLimiterOffset = perfStatsOffset - frameLimiterSize;

        ImGui::SameLine( frameLimiterOffset, 0 );

        SystemDebugView::DrawFrameLimiterMenu( const_cast<UpdateContext&>( context ) );

        ImGui::SameLine( perfStatsOffset );
        ImGui::Text( perfStats.c_str() );

        ImGui::SameLine( memStatsOffset );
        ImGui::Text( memStats.c_str() );
    }
}