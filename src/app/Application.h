#pragma once

#include "app/AppSettings.h"
#include "app/FileBrowser.h"
#include "app/Localization.h"
#include "app/McpServer.h"
#include "app/NodeGraphView.h"
#include "app/PluginManager.h"
#include "codegen/GlslGenerator.h"
#include "core/Graph.h"
#include "core/MaterialInstance.h"
#include "core/MaterialParameters.h"
#include "core/PreviewBuffer.h"
#include "core/PreviewLighting.h"
#include "project/Project.h"
#include "project/UeSync.h"
#include "render/MaterialThumbnailRenderer.h"
#include "render/PreviewRenderer.h"
#include "render/RenderApi.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

struct GLFWwindow;
struct ImFont;

namespace mf {

struct ApplicationOptions {
    std::string openPath;
    std::string projectPath;
    std::string capturePath;
    int captureFrame = 45;
    std::string rendererOverride;  // "opengl" / "vulkan" / "dx12" for diagnostics.
    bool openPreferences = false;
    bool openConsole = false;
    bool collapseLibrary = false;
    bool detachPreview = false;
    std::string dumpShaderPath;    // Writes .glsl/.hlsl next to this path and exits.
    std::string startupCommand;    // Executes one console command after startup.
};

class Application {
public:
    enum class LogLevel {
        Info,
        Warning,
        Error
    };

    struct ConsoleEntry {
        double time = 0.0;
        LogLevel level = LogLevel::Info;
        std::string message;
    };

    explicit Application(ApplicationOptions options = {});
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    bool Initialize();
    void Run();
    void Shutdown();

private:
    enum class FileAction {
        None,
        OpenGraph,
        SaveGraph,
        SaveGraphAs,
        ExportGraph,
        SelectTexture,
        OpenProject,
        AddProjectFile,
        SelectProjectLocation,
        SelectPreviewMesh,
        SelectPreviewHdri,
        ExportUeBatch,
        MoveProjectEntryTo,
        CopyProjectEntryTo,
        InstallPlugin,
        SelectPluginCreateLocation
    };

    enum class DocumentKind {
        Material,
        Function,
        Instance
    };

    struct EditorDocument {
        std::string title;
        std::filesystem::path path;
        DocumentKind kind = DocumentKind::Material;
        MaterialGraph graph;
        MaterialInstance instance;
        std::vector<MaterialParameterInfo> instanceParameters;

        std::vector<MaterialGraph> undoStack;
        std::vector<MaterialGraph> redoStack;
        MaterialGraph undoBaseline;
        MaterialGraph pendingUndoBaseline;
        std::uint64_t undoContentRevision = 0;
        std::uint64_t undoLayoutRevision = 0;
        double undoLastChangeTime = 0.0;
        bool undoInitialized = false;
        bool undoPending = false;
    };

    struct TexturePackage {
        std::string id;
        std::string name;
        std::string description;
        std::vector<std::filesystem::path> files;
        bool selected = false;
    };

    struct TexturePickerItem {
        std::filesystem::path path;
        std::string label;
        std::string relative;
        bool projectAsset = false;
        bool defaultAsset = false;
    };

    void DrawUi();
    void DrawMenuBar();
    void DrawToolbar();
    void DrawBody(float height);
    void DrawDocumentTabs(float width, float height);
    void DrawDocumentDialogs();
    void DrawThemeMenu();
    void DrawNewFunctionModal();
    void DrawPluginManager();
    void DrawCreatePluginModal();
    void RequestInstallPlugin();
    void RequestCreatePlugin();
    void CreatePluginTemplate(const std::string& parentDirectory,
                              const std::string& pluginName, const std::string& pluginId,
                              const std::string& author, const std::string& description,
                              std::string* error);
    void StartMcpServerIfEnabled();
    bool StartMcpServer(std::string* error = nullptr);
    void StopMcpServer();
    void ProcessMcpPending();
    nlohmann::json HandleMcpToolCall(const std::string& tool, const nlohmann::json& args);
    void SaveMcpSettings();
    void SyncActiveDocument();
    void RegisterLiveDocument(DocumentKind kind, const std::filesystem::path& path);
    void ActivateDocument(int index);
    void RequestCloseDocument(int index);
    void CloseDocumentNow(int index);
    void RequestNewFunction();
    bool CreateBlueprintFunction(const std::string& functionName, std::string* error = nullptr);
    void OpenFunctionAsset(const std::filesystem::path& path);
    void OpenMaterialInstance(const std::filesystem::path& path);
    bool CreateMaterialInstanceFromMaterial(const std::string& relativeMaterial,
                                             std::string* error = nullptr);
    std::filesystem::path ResolveInstanceParent(
        const MaterialInstance& instance,
        const std::filesystem::path& instancePath = {}) const;
    void ApplyUiScale(float scale);
    float ResolveAutoUiScale() const;
    void SetTheme(const std::string& theme);
    EditorDocument* ActiveEditorDocument();
    const EditorDocument* ActiveEditorDocument() const;
    void InitializeUndoForDocument(EditorDocument& document);
    void ResetUndoForActiveDocument();
    void FlushUndoPending(bool force);
    void UpdateUndoAndAutosave();
    void Undo();
    void Redo();
    bool CanUndo() const;
    bool CanRedo() const;
    std::filesystem::path AutosaveDataPath() const;
    std::filesystem::path AutosaveMetaPath() const;
    std::filesystem::path AutosaveDirectory() const;
    void WriteAutosave(bool force = false);
    void DeleteAutosave();
    void CheckAutosaveRecovery();
    void DrawAutosaveRecoveryModal();
    void RecoverAutosave();
    void DiscardAutosave();
    void DrawLeftSidebar(float width, float height);
    void DrawNodeLibraryContent();
    bool DrawMaterialFunctionLibrary(const std::string& query = {});
    void RefreshFunctionAssets();
    void OpenMaterialFunction(const std::string& path);
    void RevealMaterialFunction(const std::string& path);
    std::filesystem::path ResolveFunctionPath(const std::string& path) const;
    void DrawFilesPanel(float height);
    void DrawProjectDialogs();
    void DrawNewProjectModal();
    void DrawNewFolderModal();
    void DrawNewMaterialModal();
    void DrawRecentProjectsMenu();
    void DrawNodeEditorPanel(float width, float height);
    void DrawRightSidebar(float width, float height);
    void DrawPreviewPanel(float width, float height);
    void DrawPreviewContents(float width, float height, bool floating);
    void DrawDetachedPreviewWindow();
    void DetachPreview();
    void DockPreview();
    void SavePreviewWindowPlacement();
    void DrawDetailsPanel(float width, float height);
    void DrawNodeDetails();
    void DrawMaterialSettings();
    void DrawInstancePanel();
    void DrawPreviewSceneModal();
    void ApplyPreviewSceneSettings();
    void DrawUeBridgeModal();
    void RequestUeBatchExport();
    bool WriteUeBatchManifest(const std::filesystem::path& path, std::string* error);
    PreviewLightingSettings PreviewLighting() const;
    PreviewEnvironmentSettings PreviewEnvironment() const;
    void DrawShaderPanel();
    void DrawStatusBar();
    void DrawConsolePanel(float height);
    void DrawPreferences();
    void ProbeRenderBackends();
    void SetRenderApi(RenderApi api);
    void SetConsoleVisible(bool visible);
    void SetNodeLibraryCollapsed(bool collapsed);
    void LogMessage(LogLevel level, const std::string& message);
    void ClearConsole();
    void ExecuteConsoleCommand(const std::string& command);
    void DrawConsoleEntries();
    void DrainExternalMessages();
    void DrawAboutPopup();
    void DrawLanguageMenu();
    void LoadUiSettings();
    void SaveUiSettings();
    void SetLanguage(Language language);

    void UpdateMaterialIfNeeded();
    void RequestNewDocument();
    void RequestOpenDialog();
    void RequestSave(bool saveAs);
    void RequestExportDialog();
    void RequestTextureDialog(NodeId nodeId, const std::string& propertyKey);
    void RequestNewProject();
    void RequestOpenProject();
    void RequestNewFolder();
    void RequestNewMaterial();
    void CollapseSelectionToFunction();
    void RequestAddProjectFile();
    void SaveProject();
    void RequestTexturePackages();
    void DrawTexturePackagesModal();
    std::vector<TexturePackage> BuildTexturePackages() const;
    bool ImportTexturePackage(const TexturePackage& package, int* imported,
                              int* skipped, std::string* error);
    void OpenTexturePicker(NodeId nodeId, PinId pinId);
    void DrawTexturePickerModal();
    std::vector<TexturePickerItem> CollectTexturePickerItems();
    std::string ImportDefaultTextureIntoProject(const std::filesystem::path& source,
                                                std::string* error = nullptr);
    bool ApplyTexturePickerItem(const TexturePickerItem& item, std::string* error);
    void OpenTextureViewer(const std::filesystem::path& path);
    void DrawTextureViewer();
    void RebuildTextureViewerTexture();
    void CloseTextureViewerTexture();
    void StartNodePreview(NodeId nodeId, PinId pinId);
    void StopNodePreview();
    void RefreshNodePreview();
    void RequestRenameProjectFolder(const std::string& relativePath);
    void DrawRenameFolderModal();
    void RequestRenameProjectFile(const std::string& relativePath);
    void DrawRenameFileModal();
    void DrawDeleteEntryModal();
    void RenameProjectFolder(const std::string& relativePath, const std::string& newName,
                             std::string* error = nullptr);
    void RenameProjectFile(const std::string& relativePath, const std::string& newName,
                           std::string* error = nullptr);
    void RequestProjectEntryDestination(const std::string& relativePath, bool move);
    void RequestProjectEntryDestination(const std::vector<std::string>& relativePaths, bool move);
    void RequestDeleteProjectEntries(const std::vector<std::string>& relativePaths);
    void DrawDeleteMultipleModal();
    void MoveProjectEntries(const std::vector<std::string>& relativePaths,
                            const std::string& targetDirectory);
    void CopyProjectEntries(const std::vector<std::string>& relativePaths,
                            const std::string& targetDirectory);
    void DeleteProjectEntries(const std::vector<std::string>& relativePaths);
    std::vector<std::string> EffectiveProjectSelection() const;
    void DuplicateProjectEntry(const std::string& relativePath);
    void DuplicateProjectEntries(const std::vector<std::string>& relativePaths);
    void CopyProjectEntryTo(const std::string& relativePath,
                            const std::string& targetDirectory);
    void RemapUeSyncPrefix(const std::string& oldPrefix, const std::string& newPrefix);
    void RemoveUeSyncPrefix(const std::string& prefix);
    void SetProjectFolderColor(const std::string& relativePath, const std::string& color);
    void MoveProjectEntry(const std::string& relativePath,
                          const std::string& targetDirectory);
    void CopyProjectEntryToClipboard(const std::string& relativePath, bool move);
    void PasteProjectClipboard(const std::string& targetDirectory);
    void UpdateOpenDocumentPrefixes(const std::string& oldPrefix,
                                    const std::string& newPrefix);
    void HandleFileBrowserResult(const std::string& selectedPath);
    bool OpenProjectFile(const std::string& path, std::string* error = nullptr);
    bool CreateProject(const std::string& name, const std::string& location,
                       std::string* error = nullptr);
    void CloseProject();
    bool AddFileToProject(const std::string& sourcePath);
    static void FileDropCallback(GLFWwindow* window, int count, const char** paths);
    void QueueDroppedFiles(int count, const char** paths);
    void ProcessDroppedFiles();
    void RefreshProjectEntries(bool resetDirectory = false);
    void NavigateProjectFolder(const std::string& relativeDirectory);
    void RefreshProjectIntegrity();
    void OpenProjectMaterial(const std::string& absolutePath);
    bool CreateProjectMaterial(const std::string& materialName, std::string* error = nullptr);
    unsigned int GetThumbnailTexture(const std::filesystem::path& path);
    struct GraphThumbnail {
        struct Node {
            Vec2 position{};
            std::string type;
        };
        std::vector<Node> nodes;
        std::vector<std::pair<int, int>> links;
        bool valid = false;
        bool attempted = false;
    };
    const GraphThumbnail& GetGraphThumbnail(const std::filesystem::path& path);
    struct MaterialThumbnail {
        unsigned int texture = 0;
        int width = 0;
        int height = 0;
        bool attempted = false;
    };
    unsigned int GetMaterialThumbnail(const std::filesystem::path& path);
    bool DrawGraphThumbnail(ImDrawList* drawList, ImVec2 min, ImVec2 max,
                            const std::filesystem::path& path);
    void ClearThumbnailCache();
    void ExecuteNewDocument();
    void ResetGraphToNewMaterial(const std::string& materialName,
                                 const std::filesystem::path& documentPath);
    bool LoadDocument(const std::string& path, std::string* error);
    bool SaveDocument(const std::string& path, bool makeCurrentDocument, std::string* error);
    void UpdateProjectMainMaterialAfterSave(const std::filesystem::path& path);
    void SetStatus(const std::string& message, bool error = false);
    void HandleShortcuts();
    void UpdateWindowTitle();
    bool CaptureScreenshot(const std::string& path);
    void LoadFonts();
    void PushBoldFont();
    void PopBoldFont();
    void PushMonoFont();
    void PopMonoFont();
    std::filesystem::path DefaultSaveDirectory() const;
    void AddNodeFromLibrary(const std::string& type);

    ApplicationOptions m_Options;
    GLFWwindow* m_Window = nullptr;
    bool m_Initialized = false;
    bool m_ImGuiContextCreated = false;
    bool m_ImGuiGlfwInitialized = false;
    bool m_ImGuiOpenGLInitialized = false;
    bool m_GlfwInitialized = false;
    bool m_WantsExit = false;

    MaterialGraph m_Graph;
    MaterialInstance m_Instance;
    std::vector<MaterialParameterInfo> m_InstanceParameters;
    bool m_FocusInstanceTab = false;
    bool m_OpenPreviewSceneModal = false;
    bool m_PreviewSceneModalPending = false;
    bool m_ReturnToPreviewSceneAfterBrowse = false;
    std::string m_PreviewSceneError;
    bool m_OpenUeBridgeModal = false;
    bool m_UeBridgePending = false;
    std::string m_UeDestPath = "/Game/MaterialForge/Materials";
    std::string m_UeMaterialPath = "/Game/Materials/M_MaterialForge";
    std::string m_UePulledJsonPath;
    std::string m_UeLastBatchPath;
    std::string m_UeLastMessage;
    std::unique_ptr<NodeGraphView> m_GraphView;
    PreviewRenderer m_PreviewRenderer;
    UeSyncStore m_UeSync;

    std::vector<EditorDocument> m_Documents;
    int m_ActiveDocument = -1;
    DocumentKind m_DocumentKind = DocumentKind::Material;
    int m_PendingCloseDocument = -1;
    bool m_OpenCloseDocumentModal = false;
    bool m_PendingCloseSaveAs = false;

    double m_LastAutosaveTime = 0.0;
    bool m_AutosaveFailed = false;
    bool m_AutosaveRecoveryPending = false;
    bool m_AutosavePromptShown = false;
    std::string m_AutosaveOriginalPath;
    std::string m_AutosaveKind;
    std::string m_AutosaveTimestamp;

    GeneratedShader m_GeneratedShader;
    bool m_ShaderGenerated = false;
    bool m_ShaderCompiled = false;
    std::string m_ShaderError;
    std::uint64_t m_LastContentRevision = 0;
    bool m_ForceShaderRefresh = true;

    std::filesystem::path m_ContentRoot;
    std::filesystem::path m_DocumentPath;
    FileBrowser m_FileBrowser;
    FileAction m_FileAction = FileAction::None;
    NodeId m_TextureTargetNode = 0;
    std::string m_TextureTargetProperty;

    MaterialProject m_Project;
    RecentProjects m_RecentProjects;
    PluginManager m_PluginManager;
    bool m_RecentProjectsLoaded = false;
    AppSettings m_Settings;
    std::filesystem::path m_SettingsPath;
    float m_FileBarHeight = 292.0f;
    std::string m_ProjectCurrentDirectory;
    std::string m_ProjectSearch;
    std::vector<ProjectEntry> m_ProjectEntries;
    bool m_ProjectEntriesDirty = true;
    bool m_ScrollProjectSelection = false;
    std::string m_ProjectEntriesError;
    std::string m_ProjectSelectedEntry;
    std::set<std::string> m_ProjectSelectedEntries;
    std::string m_ProjectSelectionAnchor;
    bool m_OpenDeleteMultipleModal = false;
    bool m_DeleteMultiplePending = false;
    std::vector<std::string> m_DeleteMultipleEntries;
    std::vector<std::string> m_BrowseProjectEntries;
    bool m_OpenRenameFolderModal = false;
    bool m_RenameFolderModalPending = false;
    std::string m_RenameFolderRelative;
    std::string m_RenameFolderName;
    bool m_OpenDeleteEntryModal = false;
    bool m_DeleteEntryModalPending = false;
    std::string m_DeleteEntryRelative;
    std::string m_DeleteEntryName;
    bool m_DeleteEntryIsDirectory = false;
    std::string m_RenameFolderError;
    bool m_OpenRenameFileModal = false;
    bool m_RenameFileModalPending = false;
    std::string m_RenameFileRelative;
    std::string m_RenameFileName;
    std::string m_RenameFileError;
    bool m_OpenTexturePackages = false;
    bool m_TexturePackagesPending = false;
    std::vector<TexturePackage> m_TexturePackages;
    std::string m_TexturePackagesError;
    bool m_OpenTexturePicker = false;
    bool m_TexturePickerPending = false;
    NodeId m_TexturePickerNode = 0;
    PinId m_TexturePickerPin = 0;
    char m_TexturePickerSearch[128] = {};
    std::vector<TexturePickerItem> m_TexturePickerItems;
    bool m_TexturePickerItemsDirty = true;
    bool m_OpenTextureViewer = false;
    std::filesystem::path m_TextureViewerPath;
    std::vector<unsigned char> m_TextureViewerPixels;
    int m_TextureViewerWidth = 0;
    int m_TextureViewerHeight = 0;
    unsigned int m_TextureViewerTexture = 0;
    int m_TextureViewerChannel = 0;
    float m_TextureViewerZoom = 1.0f;
    ImVec2 m_TextureViewerPan{};
    bool m_TextureViewerNeedsTexture = false;
    std::string m_TextureViewerError;
    bool m_NodePreviewActive = false;
    NodeId m_NodePreviewNode = 0;
    PinId m_NodePreviewPin = 0;
    std::uint64_t m_NodePreviewRevision = 0;
    MaterialGraph m_NodePreviewGraph;
    bool m_OpenPluginManager = false;
    bool m_PluginManagerPending = false;
    bool m_OpenCreatePlugin = false;
    bool m_PluginCreateModalPending = false;
    std::string m_PluginCreateName;
    std::string m_PluginCreateId;
    std::string m_PluginCreateAuthor;
    std::string m_PluginCreateDescription;
    std::string m_PluginCreateLocation;
    std::string m_PluginCreateError;
    bool m_ReturnToPluginCreateAfterBrowse = false;
    char m_PreferencesSearch[128] = {};
    std::unique_ptr<McpServer> m_McpServer;
    std::string m_McpStatus;
    std::string m_ProjectClipboardPath;
    bool m_ProjectClipboardMove = false;

    std::vector<std::string> m_PendingDroppedFiles;
    bool m_DropOnContentBrowser = false;
    bool m_ContentBrowserRectValid = false;
    float m_ContentBrowserMinX = 0.0f;
    float m_ContentBrowserMinY = 0.0f;
    float m_ContentBrowserMaxX = 0.0f;
    float m_ContentBrowserMaxY = 0.0f;

    struct ThumbnailEntry {
        unsigned int texture = 0;
        int width = 0;
        int height = 0;
        bool attempted = false;
    };
    std::unordered_map<std::string, ThumbnailEntry> m_Thumbnails;
    std::unordered_map<std::string, GraphThumbnail> m_GraphThumbnails;
    std::unordered_map<std::string, MaterialThumbnail> m_MaterialThumbnails;
    std::unique_ptr<MaterialThumbnailRenderer> m_MaterialThumbnailRenderer;

    bool m_OpenNewProjectModal = false;
    bool m_NewProjectModalPending = false;
    bool m_ReturnToNewProjectAfterBrowse = false;
    bool m_OpenNewFolderModal = false;
    bool m_NewFolderModalPending = false;
    bool m_OpenNewMaterialModal = false;
    bool m_NewMaterialModalPending = false;
    bool m_OpenNewFunctionModal = false;
    bool m_NewFunctionModalPending = false;
    std::string m_NewProjectName;
    std::string m_NewProjectLocation;
    std::string m_NewProjectError;
    std::string m_NewFolderName;
    std::string m_NewFolderError;
    std::string m_NewMaterialName;
    std::string m_NewMaterialError;
    std::string m_NewFunctionName;
    std::string m_NewFunctionError;

    std::string m_Theme = "graphite";
    float m_UiScale = 1.0f;
    float m_RequestedUiScale = 0.0f;  // 0 = automatic

    ImFont* m_BoldFont = nullptr;
    ImFont* m_MonoFont = nullptr;
    ImFont* m_SmallFont = nullptr;

    bool m_OpenAbout = false;
    bool m_FocusShaderTab = false;
    bool m_FocusMaterialSettingsTab = false;
    bool m_OpenPreferences = false;
    char m_LibrarySearch[128] = {};
    float m_PreviewSplit = 0.47f;

    struct FunctionAssetInfo {
        std::string name;
        std::string path;
        bool builtIn = false;
    };
    std::vector<FunctionAssetInfo> m_FunctionAssets;
    bool m_FunctionAssetsDirty = true;

    bool m_NodeLibraryCollapsed = false;
    bool m_ConsoleVisible = false;
    float m_ConsoleHeight = 190.0f;
    int m_ConsoleFilter = 0;  // 0 all, 1 info, 2 warning, 3 error.
    char m_ConsoleInput[512] = {};
    std::vector<ConsoleEntry> m_ConsoleEntries;
    std::vector<std::string> m_ConsoleHistory;
    int m_ConsoleHistoryCursor = -1;
    bool m_ConsoleScrollToBottom = false;
    bool m_ConsoleFocusInput = false;
    std::string m_LastShaderDiagnosticKey;
    std::string m_LastTextureDiagnosticKey;

    bool m_PreviewDetached = false;
    bool m_PreviewRenderedThisFrame = false;
    bool m_PreviewLive = true;
    bool m_PreviewNeedsRender = true;
    int m_PreviewLastWidth = 0;
    int m_PreviewLastHeight = 0;
    PreviewBuffer m_PreviewBuffer = PreviewBuffer::Lit;
    bool m_PixelProbeEnabled = false;
    bool m_DetachedPreviewPlacementPending = false;
    bool m_DetachedPreviewDragging = false;
    bool m_PreviewDockRectValid = false;
    float m_PreviewDockMinX = 0.0f;
    float m_PreviewDockMinY = 0.0f;
    float m_PreviewDockMaxX = 0.0f;
    float m_PreviewDockMaxY = 0.0f;
    float m_DetachedPreviewX = 0.0f;
    float m_DetachedPreviewY = 0.0f;
    float m_DetachedPreviewWidth = 560.0f;
    float m_DetachedPreviewHeight = 430.0f;
    float m_LastDetachedWindowX = 0.0f;
    float m_LastDetachedWindowY = 0.0f;

    bool m_BackendProbeDone = false;
    bool m_BackendProbeOk[3] = {true, false, false};
    std::string m_BackendProbeText[3];
    std::string m_BackendProbeError[3];

    std::string m_GlVersionString;
    int m_GlMajor = 3;
    int m_GlMinor = 3;
    int m_GlslVersion = 330;

    std::string m_StatusMessage = "Ready";
    bool m_StatusError = false;
    double m_StatusExpiresAt = 0.0;

    double m_TimeSeconds = 0.0;
    double m_StartTime = 0.0;
    int m_FrameCounter = 0;
    int m_CaptureFrame = -1;
    std::string m_CapturePath;
    bool m_CaptureWritten = false;
};

} // namespace mf
