package dev.encounter.borealis;

import android.app.ActionBar;
import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.ClipData;
import android.content.Context;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.provider.Settings;
import android.util.Log;
import android.webkit.MimeTypeMap;
import android.view.Display;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;

import dev.encounter.aurora.AuroraSurface;

import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLSurface;

import java.io.File;
import java.io.FileNotFoundException;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

public class BorealisActivity extends SDLActivity {
    private static final String TAG = "BorealisActivity";
    private static final float DEFAULT_SURFACE_FRAME_RATE = 60.0f;
    private static final int FOLDER_DIALOG_REQUEST_CODE = 0x4253;
    private static final int MANAGE_STORAGE_REQUEST_CODE = 0x4254;
    private static final int EXPORT_DIALOG_REQUEST_CODE = 0x4255;
    private static final String EXTERNAL_STORAGE_AUTHORITY =
        "com.android.externalstorage.documents";

    private long folderDialogUserdata = 0;
    private boolean folderDialogRequiresRealPath = false;
    private boolean awaitingManageStoragePermission = false;
    private long exportDialogUserdata = 0;

    private static native void nativeFolderDialogResult(
        long userdata, String path, String error);
    private static native void nativeExportDialogResult(
        long userdata, String path, String error);

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        hideSystemBars();
    }

    @Override
    protected SDLSurface createSDLSurface(Context context) {
        return new BorealisSurface(context);
    }

    @Override
    protected void onResume() {
        super.onResume();
        hideSystemBars();
        if (awaitingManageStoragePermission) {
            resumeFolderDialogAfterPermissionGrant();
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (resultCode == RESULT_OK) {
            persistUriPermissions(data);
        }
        if (requestCode == FOLDER_DIALOG_REQUEST_CODE) {
            finishFolderDialog(resultCode, data);
            return;
        }
        if (requestCode == EXPORT_DIALOG_REQUEST_CODE) {
            finishExportDialog(resultCode, data);
            return;
        }
        super.onActivityResult(requestCode, resultCode, data);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            hideSystemBars();
        }
    }

    @Override
    protected String[] getLibraries() {
        return new String[] { "main" };
    }

    @Override
    protected String[] getArguments() {
        Intent intent = getIntent();
        if (intent == null) {
            return new String[0];
        }

        String[] argv = intent.getStringArrayExtra("borealis_argv");
        if (argv != null && argv.length > 0) {
            return argv;
        }

        String rawArgs = intent.getStringExtra("borealis_args");
        return rawArgs == null ? new String[0] : splitArguments(rawArgs.trim());
    }

    protected static String[] splitArguments(String raw) {
        if (raw.isEmpty()) {
            return new String[0];
        }

        List<String> out = new ArrayList<>();
        StringBuilder current = new StringBuilder();
        boolean inSingle = false;
        boolean inDouble = false;
        boolean escaped = false;

        for (int i = 0; i < raw.length(); ++i) {
            char c = raw.charAt(i);
            if (escaped) {
                current.append(c);
                escaped = false;
                continue;
            }
            if (c == '\\' && !inSingle) {
                escaped = true;
                continue;
            }
            if (c == '"' && !inSingle) {
                inDouble = !inDouble;
                continue;
            }
            if (c == '\'' && !inDouble) {
                inSingle = !inSingle;
                continue;
            }
            if (!inSingle && !inDouble && Character.isWhitespace(c)) {
                if (current.length() > 0) {
                    out.add(current.toString());
                    current.setLength(0);
                }
                continue;
            }
            current.append(c);
        }

        if (escaped) {
            current.append('\\');
        }
        if (current.length() > 0) {
            out.add(current.toString());
        }
        return out.toArray(new String[0]);
    }

    /** Called by borealis::file_select through JNI. */
    public boolean showFolderDialog(long userdata, boolean requireRealPath) {
        if (userdata == 0 || folderDialogUserdata != 0) {
            return false;
        }

        folderDialogUserdata = userdata;
        folderDialogRequiresRealPath = requireRealPath;
        if (requireRealPath && requiresManageStoragePermission() && !hasManageStoragePermission()) {
            requestManageStoragePermission();
            return true;
        }

        openFolderDialog();
        return true;
    }

    private void openFolderDialog() {
        runOnUiThread(() -> {
            Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
            intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION |
                Intent.FLAG_GRANT_WRITE_URI_PERMISSION |
                Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION |
                Intent.FLAG_GRANT_PREFIX_URI_PERMISSION);

            try {
                startActivityForResult(intent, FOLDER_DIALOG_REQUEST_CODE);
            } catch (ActivityNotFoundException e) {
                Log.w(TAG, "Unable to open folder dialog.", e);
                finishFolderDialogWithError("Unable to open the Android folder dialog");
            }
        });
    }

    private boolean requiresManageStoragePermission() {
        return Build.VERSION.SDK_INT >= Build.VERSION_CODES.R;
    }

    private boolean hasManageStoragePermission() {
        return !requiresManageStoragePermission() || Environment.isExternalStorageManager();
    }

    private void requestManageStoragePermission() {
        awaitingManageStoragePermission = true;
        runOnUiThread(() -> {
            if (tryStartManageStorageIntent(
                    new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION)
                        .setData(Uri.parse("package:" + getPackageName()))) ||
                tryStartManageStorageIntent(
                    new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION)))
            {
                return;
            }

            finishFolderDialogWithError(
                "Unable to request Android all-files access permission");
        });
    }

    private boolean tryStartManageStorageIntent(Intent intent) {
        try {
            startActivityForResult(intent, MANAGE_STORAGE_REQUEST_CODE);
            return true;
        } catch (ActivityNotFoundException e) {
            Log.w(TAG, "Unable to open all-files access settings.", e);
            return false;
        }
    }

    private void resumeFolderDialogAfterPermissionGrant() {
        awaitingManageStoragePermission = false;
        if (folderDialogUserdata == 0) {
            return;
        }
        if (hasManageStoragePermission()) {
            openFolderDialog();
            return;
        }
        finishFolderDialogWithError("Android all-files access permission was not granted");
    }

    private void finishFolderDialogWithError(String error) {
        long userdata = folderDialogUserdata;
        folderDialogUserdata = 0;
        folderDialogRequiresRealPath = false;
        awaitingManageStoragePermission = false;
        if (userdata != 0) {
            nativeFolderDialogResult(userdata, null, error);
        }
    }

    private void finishFolderDialog(int resultCode, Intent data) {
        long userdata = folderDialogUserdata;
        folderDialogUserdata = 0;
        boolean requireRealPath = folderDialogRequiresRealPath;
        folderDialogRequiresRealPath = false;
        if (userdata == 0) {
            return;
        }

        if (resultCode == Activity.RESULT_OK && data != null && data.getData() != null) {
            Uri uri = data.getData();
            if (!requireRealPath) {
                nativeFolderDialogResult(userdata, uri.toString(), null);
                return;
            }
            String path = getRealPathForUri(uri);
            if (path == null || path.isEmpty()) {
                nativeFolderDialogResult(userdata, null,
                    "Selected folder is not available as a filesystem path");
                return;
            }
            nativeFolderDialogResult(userdata, path, null);
            return;
        }
        nativeFolderDialogResult(userdata, null, null);
    }

    /** Called by borealis::file_select through JNI. */
    public boolean showExportDialog(long userdata, String suggestedName, String[] filterPatterns) {
        if (userdata == 0 || exportDialogUserdata != 0) {
            return false;
        }
        exportDialogUserdata = userdata;
        runOnUiThread(() -> {
            Intent intent = new Intent(Intent.ACTION_CREATE_DOCUMENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION |
                Intent.FLAG_GRANT_WRITE_URI_PERMISSION |
                Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
            intent.putExtra(Intent.EXTRA_TITLE, suggestedName);
            String[] mimeTypes = mimeTypesForPatterns(filterPatterns);
            intent.setType(mimeTypes.length == 1 ? mimeTypes[0] : "*/*");
            if (mimeTypes.length > 1) {
                intent.putExtra(Intent.EXTRA_MIME_TYPES, mimeTypes);
            }
            try {
                startActivityForResult(intent, EXPORT_DIALOG_REQUEST_CODE);
            } catch (ActivityNotFoundException e) {
                Log.w(TAG, "Unable to open export dialog.", e);
                finishExportDialogWithError("Unable to open the Android export dialog");
            }
        });
        return true;
    }

    private void finishExportDialog(int resultCode, Intent data) {
        long userdata = exportDialogUserdata;
        exportDialogUserdata = 0;
        if (userdata == 0) {
            return;
        }
        String path = resultCode == Activity.RESULT_OK && data != null && data.getData() != null
            ? data.getData().toString()
            : null;
        // Copying can be large; enter native code from a Java-owned worker thread.
        new Thread(() -> nativeExportDialogResult(userdata, path, null),
            "Borealis file export").start();
    }

    private void finishExportDialogWithError(String error) {
        long userdata = exportDialogUserdata;
        exportDialogUserdata = 0;
        if (userdata != 0) {
            nativeExportDialogResult(userdata, null, error);
        }
    }

    private String getRealPathForUri(Uri uri) {
        if (uri == null) {
            return null;
        }
        if ("file".equals(uri.getScheme())) {
            return uri.getPath();
        }
        if (!"content".equals(uri.getScheme()) ||
            !EXTERNAL_STORAGE_AUTHORITY.equals(uri.getAuthority()) ||
            Build.VERSION.SDK_INT < Build.VERSION_CODES.KITKAT)
        {
            return null;
        }

        try {
            return getExternalStoragePathForDocumentId(getExternalStorageDocumentId(uri));
        } catch (IllegalArgumentException e) {
            Log.w(TAG, "Unable to resolve URI: " + uri, e);
            return null;
        }
    }

    private static String getExternalStorageDocumentId(Uri uri) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP && isTreeDocumentUri(uri)) {
            return DocumentsContract.getTreeDocumentId(uri);
        }
        return DocumentsContract.getDocumentId(uri);
    }

    private static boolean isTreeDocumentUri(Uri uri) {
        List<String> segments = uri.getPathSegments();
        return segments.size() >= 2 && "tree".equals(segments.get(0));
    }

    private String getExternalStoragePathForDocumentId(String documentId) {
        if (documentId == null || documentId.isEmpty()) {
            return null;
        }
        if (documentId.startsWith("raw:")) {
            return documentId.substring("raw:".length());
        }

        String[] parts = documentId.split(":", 2);
        String volumeId = parts[0];
        String relativePath = parts.length > 1 ? parts[1] : "";
        File root = getExternalStorageRoot(volumeId);
        if (root == null) {
            return null;
        }
        return relativePath.isEmpty()
            ? root.getAbsolutePath()
            : new File(root, relativePath).getAbsolutePath();
    }

    private File getExternalStorageRoot(String volumeId) {
        if ("primary".equalsIgnoreCase(volumeId)) {
            return Environment.getExternalStorageDirectory();
        }
        if ("home".equalsIgnoreCase(volumeId)) {
            return new File(
                Environment.getExternalStorageDirectory(), Environment.DIRECTORY_DOCUMENTS);
        }

        File[] externalFilesDirs = getExternalFilesDirs(null);
        if (externalFilesDirs != null) {
            for (File externalFilesDir : externalFilesDirs) {
                File root = getStorageRootForExternalFilesDir(externalFilesDir);
                if (root != null && volumeId.equalsIgnoreCase(root.getName())) {
                    return root;
                }
            }
        }

        File fallback = new File("/storage", volumeId);
        return fallback.exists() ? fallback : null;
    }

    private File getStorageRootForExternalFilesDir(File externalFilesDir) {
        if (externalFilesDir == null) {
            return null;
        }
        String path = externalFilesDir.getAbsolutePath();
        int androidDir = path.indexOf("/Android/");
        return androidDir <= 0 ? null : new File(path.substring(0, androidDir));
    }

    private void persistUriPermissions(Intent data) {
        if (data == null) {
            return;
        }

        int permissionFlags = data.getFlags() &
            (Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        if (permissionFlags == 0) {
            return;
        }

        Uri uri = data.getData();
        if (uri != null) {
            persistUriPermission(uri, permissionFlags);
        }

        ClipData clipData = data.getClipData();
        if (clipData == null) {
            return;
        }
        for (int i = 0; i < clipData.getItemCount(); ++i) {
            Uri itemUri = clipData.getItemAt(i).getUri();
            if (itemUri != null) {
                persistUriPermission(itemUri, permissionFlags);
            }
        }
    }

    private void persistUriPermission(Uri uri, int permissionFlags) {
        if ((permissionFlags & Intent.FLAG_GRANT_READ_URI_PERMISSION) != 0) {
            persistUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION, "read");
        }
        if ((permissionFlags & Intent.FLAG_GRANT_WRITE_URI_PERMISSION) != 0) {
            persistUriPermission(uri, Intent.FLAG_GRANT_WRITE_URI_PERMISSION, "write");
        }
    }

    private void persistUriPermission(Uri uri, int permissionFlag, String permissionName) {
        try {
            getContentResolver().takePersistableUriPermission(uri, permissionFlag);
        } catch (SecurityException | IllegalArgumentException e) {
            Log.w(TAG, "Unable to persist " + permissionName +
                " URI permission for " + uri, e);
        }
    }

    /** Called by borealis::file_select to label Android content URIs. */
    public String getDisplayNameForUri(String uriString) {
        if (uriString == null || uriString.isEmpty()) {
            return "";
        }

        Uri uri = Uri.parse(uriString);
        if ("content".equals(uri.getScheme())) {
            Uri queryUri = documentUriFor(uri);
            try (Cursor cursor = getContentResolver().query(
                queryUri, new String[] { OpenableColumns.DISPLAY_NAME }, null, null, null))
            {
                if (cursor != null && cursor.moveToFirst()) {
                    int column = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                    if (column >= 0) {
                        String displayName = cursor.getString(column);
                        if (displayName != null && !displayName.isEmpty()) {
                            return displayName;
                        }
                    }
                }
            } catch (SecurityException | IllegalArgumentException e) {
                Log.w(TAG, "Unable to query display name for " + uri, e);
            }
        } else if ("file".equals(uri.getScheme())) {
            String path = uri.getPath();
            if (path != null && !path.isEmpty()) {
                String name = new File(path).getName();
                if (!name.isEmpty()) {
                    return name;
                }
            }
        }

        String lastSegment = uri.getLastPathSegment();
        return lastSegment != null ? lastSegment : "";
    }

    /** Check persisted URI access. */
    public boolean checkUri(String uriString) {
        if (uriString == null || uriString.isEmpty()) {
            return false;
        }
        Uri uri = Uri.parse(uriString);
        if ("file".equals(uri.getScheme())) {
            return uri.getPath() != null && new File(uri.getPath()).exists();
        }
        if (!"content".equals(uri.getScheme())) {
            return false;
        }
        try (Cursor cursor = getContentResolver().query(documentUriFor(uri),
            new String[] { DocumentsContract.Document.COLUMN_DOCUMENT_ID }, null, null, null))
        {
            return cursor != null && cursor.moveToFirst();
        } catch (SecurityException | IllegalArgumentException e) {
            Log.w(TAG, "Unable to access document URI " + uri, e);
            return false;
        }
    }

    /** Opens a content URI and transfers ownership of its descriptor to native code. */
    public int openUriFileDescriptor(String uriString, String mode) {
        if (uriString == null || uriString.isEmpty() || mode == null) {
            return -1;
        }
        try {
            ParcelFileDescriptor descriptor =
                getContentResolver().openFileDescriptor(Uri.parse(uriString), mode);
            return descriptor != null ? descriptor.detachFd() : -1;
        } catch (FileNotFoundException | SecurityException | IllegalArgumentException e) {
            Log.w(TAG, "Unable to open document URI " + uriString, e);
            return -1;
        }
    }

    /** Resolve an existing descendant. */
    public String joinDocumentUri(String folderString, String relativePath) {
        if (folderString == null || relativePath == null) {
            return null;
        }
        Uri treeUri = Uri.parse(folderString);
        if (!"content".equals(treeUri.getScheme()) || !isTreeDocumentUri(treeUri)) {
            return null;
        }

        Uri current = documentUriFor(treeUri);
        for (String segment : relativePath.split("[/\\\\]")) {
            if (segment.isEmpty() || ".".equals(segment) || "..".equals(segment)) {
                return null;
            }
            current = findDocumentChild(treeUri, current, segment);
            if (current == null) {
                return null;
            }
        }
        return current.toString();
    }

    /** Creates one document in a selected tree and returns its provider-authoritative URI. */
    public String createDocumentUri(String folderString, String displayName) {
        if (folderString == null || displayName == null || displayName.isEmpty()) {
            return null;
        }
        Uri treeUri = Uri.parse(folderString);
        if (!"content".equals(treeUri.getScheme()) || !isTreeDocumentUri(treeUri)) {
            return null;
        }
        try {
            Uri child = DocumentsContract.createDocument(getContentResolver(),
                documentUriFor(treeUri), mimeTypeForName(displayName), displayName);
            return child != null ? child.toString() : null;
        } catch (FileNotFoundException | SecurityException | IllegalArgumentException e) {
            Log.w(TAG, "Unable to create document " + displayName, e);
            return null;
        }
    }

    /** Removes a destination that could not be fully exported. */
    public boolean deleteDocumentUri(String uriString) {
        if (uriString == null || uriString.isEmpty()) {
            return false;
        }
        try {
            return DocumentsContract.deleteDocument(
                getContentResolver(), Uri.parse(uriString));
        } catch (FileNotFoundException | SecurityException | IllegalArgumentException e) {
            Log.w(TAG, "Unable to remove incomplete document " + uriString, e);
            return false;
        }
    }

    private static String mimeTypeForName(String displayName) {
        int separator = displayName.lastIndexOf('.');
        if (separator >= 0 && separator + 1 < displayName.length()) {
            String extension = displayName.substring(separator + 1).toLowerCase(Locale.ROOT);
            String type = MimeTypeMap.getSingleton().getMimeTypeFromExtension(extension);
            if (type != null && !type.isEmpty()) {
                return type;
            }
        }
        return "application/octet-stream";
    }

    private static String[] mimeTypesForPatterns(String[] patterns) {
        ArrayList<String> result = new ArrayList<>();
        if (patterns != null) {
            for (String pattern : patterns) {
                if (pattern == null) {
                    continue;
                }
                for (String extension : pattern.split(";")) {
                    String normalized = extension.trim();
                    while (normalized.startsWith("*.")) {
                        normalized = normalized.substring(2);
                    }
                    while (normalized.startsWith(".")) {
                        normalized = normalized.substring(1);
                    }
                    String type = MimeTypeMap.getSingleton().getMimeTypeFromExtension(
                        normalized.toLowerCase(Locale.ROOT));
                    if (type != null && !result.contains(type)) {
                        result.add(type);
                    }
                }
            }
        }
        return result.toArray(new String[0]);
    }

    /** Returns flat name, URI, directory triples for JNI. */
    public String[] listDocumentUri(String folderString) {
        if (folderString == null) {
            return null;
        }
        Uri treeUri = Uri.parse(folderString);
        if (!"content".equals(treeUri.getScheme()) || !isTreeDocumentUri(treeUri)) {
            return null;
        }
        Uri folder = documentUriFor(treeUri);
        String folderId;
        try {
            folderId = DocumentsContract.getDocumentId(folder);
        } catch (IllegalArgumentException e) {
            return null;
        }
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(treeUri, folderId);
        ArrayList<String> result = new ArrayList<>();
        String[] projection = {
            DocumentsContract.Document.COLUMN_DOCUMENT_ID,
            DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            DocumentsContract.Document.COLUMN_MIME_TYPE,
        };
        try (Cursor cursor = getContentResolver().query(children, projection, null, null, null)) {
            if (cursor == null) {
                return null;
            }
            int idColumn = cursor.getColumnIndex(
                DocumentsContract.Document.COLUMN_DOCUMENT_ID);
            int nameColumn = cursor.getColumnIndex(
                DocumentsContract.Document.COLUMN_DISPLAY_NAME);
            int typeColumn = cursor.getColumnIndex(
                DocumentsContract.Document.COLUMN_MIME_TYPE);
            while (cursor.moveToNext()) {
                String id = cursor.getString(idColumn);
                String name = cursor.getString(nameColumn);
                String type = cursor.getString(typeColumn);
                if (id == null || name == null) {
                    continue;
                }
                result.add(name);
                result.add(DocumentsContract.buildDocumentUriUsingTree(treeUri, id).toString());
                result.add(DocumentsContract.Document.MIME_TYPE_DIR.equals(type) ? "1" : "0");
            }
        } catch (SecurityException | IllegalArgumentException e) {
            Log.w(TAG, "Unable to list document URI " + treeUri, e);
            return null;
        }
        return result.toArray(new String[0]);
    }

    private Uri documentUriFor(Uri uri) {
        if (!isTreeDocumentUri(uri)) {
            return uri;
        }
        try {
            List<String> segments = uri.getPathSegments();
            if (segments.size() >= 4 && "document".equals(segments.get(2))) {
                return uri;
            }
            return DocumentsContract.buildDocumentUriUsingTree(
                uri, DocumentsContract.getTreeDocumentId(uri));
        } catch (IllegalArgumentException e) {
            return uri;
        }
    }

    private Uri findDocumentChild(Uri treeUri, Uri folder, String displayName) {
        String folderId;
        try {
            folderId = DocumentsContract.getDocumentId(folder);
        } catch (IllegalArgumentException e) {
            return null;
        }
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(treeUri, folderId);
        String[] projection = {
            DocumentsContract.Document.COLUMN_DOCUMENT_ID,
            DocumentsContract.Document.COLUMN_DISPLAY_NAME,
        };
        // Some document providers do not implement query selection arguments.
        try (Cursor cursor = getContentResolver().query(
            children, projection, null, null, null))
        {
            if (cursor == null) {
                return null;
            }
            int idColumn = cursor.getColumnIndex(
                DocumentsContract.Document.COLUMN_DOCUMENT_ID);
            int nameColumn = cursor.getColumnIndex(
                DocumentsContract.Document.COLUMN_DISPLAY_NAME);
            while (cursor.moveToNext()) {
                if (displayName.equals(cursor.getString(nameColumn))) {
                    return DocumentsContract.buildDocumentUriUsingTree(
                        treeUri, cursor.getString(idColumn));
                }
            }
        } catch (SecurityException | IllegalArgumentException e) {
            Log.w(TAG, "Unable to resolve child " + displayName + " in " + folder, e);
        }
        return null;
    }

    public void setPreferredSurfaceFrameRate(float frameRate) {
        runOnUiThread(() -> {
            if (mSurface instanceof BorealisSurface) {
                ((BorealisSurface)mSurface).setPreferredFrameRate(frameRate);
            }
        });
    }

    private void hideSystemBars() {
        Window window = getWindow();
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            window.setDecorFitsSystemWindows(false);
            WindowInsetsController controller =
                window.getDecorView().getWindowInsetsController();
            if (controller != null) {
                controller.setSystemBarsBehavior(
                    WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
                controller.hide(WindowInsets.Type.systemBars());
            }
            return;
        }

        View decorView = window.getDecorView();
        int options = View.SYSTEM_UI_FLAG_FULLSCREEN |
            View.SYSTEM_UI_FLAG_HIDE_NAVIGATION |
            View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY |
            View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN |
            View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION |
            View.SYSTEM_UI_FLAG_LAYOUT_STABLE;
        decorView.setSystemUiVisibility(options);
        ActionBar actionBar = getActionBar();
        if (actionBar != null) {
            actionBar.hide();
        }
    }

    private static final class BorealisSurface extends AuroraSurface {
        private float preferredFrameRate = DEFAULT_SURFACE_FRAME_RATE;

        BorealisSurface(Context context) {
            super(context);
        }

        @Override
        public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
            super.surfaceChanged(holder, format, width, height);
            setTargetFrameRate(holder);
        }

        void setPreferredFrameRate(float frameRate) {
            preferredFrameRate = frameRate;
            setTargetFrameRate(getHolder());
        }

        private void setTargetFrameRate(SurfaceHolder holder) {
            if (!mIsSurfaceReady || Build.VERSION.SDK_INT < Build.VERSION_CODES.R) {
                return;
            }

            Surface surface = holder != null ? holder.getSurface() : getHolder().getSurface();
            if (surface == null || !surface.isValid()) {
                return;
            }

            float targetFrameRate = preferredFrameRate > 0.0f
                ? preferredFrameRate
                : getMaxSupportedFrameRate();
            if (targetFrameRate <= 0.0f) {
                return;
            }

            try {
                surface.setFrameRate(
                    targetFrameRate, Surface.FRAME_RATE_COMPATIBILITY_DEFAULT);
                Log.v(TAG, "Requested surface frame rate " + targetFrameRate + " fps");
            } catch (RuntimeException e) {
                Log.w(TAG, "Failed to request surface frame rate", e);
            }
        }

        private float getMaxSupportedFrameRate() {
            if (mDisplay == null) {
                return 0.0f;
            }

            float maxFrameRate = mDisplay.getRefreshRate();
            Display.Mode[] modes = mDisplay.getSupportedModes();
            if (modes != null) {
                for (Display.Mode mode : modes) {
                    maxFrameRate = Math.max(maxFrameRate, mode.getRefreshRate());
                }
            }
            return maxFrameRate;
        }
    }
}
