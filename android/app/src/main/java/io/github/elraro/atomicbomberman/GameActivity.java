package io.github.elraro.atomicbomberman;

import android.app.Activity;
import android.content.ContentResolver;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.UriPermission;
import android.database.Cursor;
import android.net.Uri;
import android.provider.DocumentsContract;

import org.libsdl.app.SDLActivity;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.Locale;

/**
 * SDL's activity plus the one thing the game cannot do from C++: letting the user choose the
 * folder that holds their copy of the original game. Android does not give a program the files
 * of a chosen folder by path, so the files the game needs are copied from it into a folder of
 * the app ("staging"), where the C++ importer converts them. The choice is remembered, so that
 * a new release can read the folder again by itself.
 *
 * The static methods are called from the game's thread (src/app/original_folder.cpp).
 */
public class GameActivity extends SDLActivity {
    private static final int REQUEST_FOLDER = 4711;
    private static final String PREFS = "original-game";
    private static final String KEY_FOLDER = "folder";

    // 0 nothing asked, 1 the chooser is open, 2 a folder was chosen, 3 the chooser was closed without
    private static volatile int pickState = 0;
    // 0 idle, 1 copying, 2 done, 3 failed
    private static volatile int stageState = 0;
    private static volatile int stageDone = 0;
    private static volatile int stageTotal = 0;
    private static volatile String stageItem = "";
    private static volatile String stageError = "";
    private static volatile boolean stageStop = false;

    private static GameActivity self() {
        return (GameActivity) mSingleton;
    }

    private static SharedPreferences prefs() {
        return self().getSharedPreferences(PREFS, MODE_PRIVATE);
    }

    public static void pickFolder() {
        final GameActivity a = self();
        pickState = 1;
        a.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                try {
                    Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
                    intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
                    a.startActivityForResult(intent, REQUEST_FOLDER);
                } catch (Exception e) {
                    pickState = 3;  // no chooser on this device
                }
            }
        });
    }

    public static int pickState() {
        return pickState;
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQUEST_FOLDER) return;
        Uri tree = resultCode == Activity.RESULT_OK && data != null ? data.getData() : null;
        if (tree == null) {
            pickState = 3;
            return;
        }
        ContentResolver resolver = getContentResolver();
        try {
            // The permission is kept over restarts; the one for a folder chosen before is given back.
            resolver.takePersistableUriPermission(tree, Intent.FLAG_GRANT_READ_URI_PERMISSION);
            for (UriPermission p : resolver.getPersistedUriPermissions())
                if (!p.getUri().equals(tree)) resolver.releasePersistableUriPermission(p.getUri(), Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (Exception e) {
            // Still readable until the app is closed.
        }
        prefs().edit().putString(KEY_FOLDER, tree.toString()).apply();
        pickState = 2;
    }

    /** The remembered folder as Android names it (a content:// address); empty if none. */
    public static String savedFolder() {
        return prefs().getString(KEY_FOLDER, "");
    }

    /** The same in words a person can read, as far as that can be told ("primary:Games/Atomic"). */
    public static String savedFolderName() {
        String saved = savedFolder();
        if (saved.isEmpty()) return "";
        try {
            String id = DocumentsContract.getTreeDocumentId(Uri.parse(saved));
            if (id.startsWith("primary:")) return "Internal storage/" + id.substring(8);
            return id;
        } catch (Exception e) {
            return saved;
        }
    }

    public static void forgetFolder() {
        prefs().edit().remove(KEY_FOLDER).apply();
    }

    // What the importer takes (src/resources/asset_import.cpp); the rest of the disc is left alone.
    private static boolean wanted(String name) {
        String n = name.toLowerCase(Locale.ROOT);
        if (n.equals("bmintro.exe")) return true;
        int dot = n.lastIndexOf('.');
        if (dot < 0) return false;
        switch (n.substring(dot + 1)) {
            case "pal": case "rmp": case "fon": case "bm": case "pcx": case "res": case "cam":
            case "sch": case "ani": case "ali": case "rss":
                return true;
            default:
                return false;
        }
    }

    private static final class Entry {
        final Uri uri;
        final String path;  // below the chosen folder
        Entry(Uri uri, String path) {
            this.uri = uri;
            this.path = path;
        }
    }

    private static void list(ContentResolver resolver, Uri tree, String documentId, String path, int depth, ArrayList<Entry> out) {
        if (depth > 6 || stageStop) return;
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, documentId);
        ArrayList<String[]> folders = new ArrayList<>();
        try (Cursor c = resolver.query(children,
                new String[] {DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                        DocumentsContract.Document.COLUMN_MIME_TYPE},
                null, null, null)) {
            while (c != null && c.moveToNext()) {
                String id = c.getString(0), name = c.getString(1), mime = c.getString(2);
                if (name == null || name.contains("/")) continue;
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(mime))
                    folders.add(new String[] {id, name});
                else if (wanted(name))
                    out.add(new Entry(DocumentsContract.buildDocumentUriUsingTree(tree, id), path + name));
            }
        }
        for (String[] f : folders) list(resolver, tree, f[0], path + f[1] + "/", depth + 1, out);
    }

    // About half of the disc's sounds are in no list of the game and are not converted: they
    // are not fetched either. The list is the game's own (soundlst.res: "number,name" lines,
    // ';' starts a comment). Without it everything is kept.
    private static ArrayList<Entry> withoutUnusedSounds(ContentResolver resolver, ArrayList<Entry> files) {
        Entry listFile = null;
        for (Entry e : files)
            if (e.path.toLowerCase(Locale.ROOT).endsWith("/soundlst.res")) listFile = e;
        if (listFile == null) return files;
        HashSet<String> listed = new HashSet<>();
        try (BufferedReader in = new BufferedReader(new InputStreamReader(resolver.openInputStream(listFile.uri), "ISO-8859-1"))) {
            String line;
            while ((line = in.readLine()) != null) {
                int semi = line.indexOf(';');
                if (semi >= 0) line = line.substring(0, semi);
                int comma = line.indexOf(',');
                if (comma < 0) continue;
                String name = line.substring(comma + 1).replaceAll("\\s", "").toLowerCase(Locale.ROOT);
                if (!name.isEmpty()) listed.add(name);
            }
        } catch (Exception e) {
            return files;
        }
        if (listed.isEmpty()) return files;
        ArrayList<Entry> out = new ArrayList<>();
        for (Entry e : files) {
            String n = e.path.substring(e.path.lastIndexOf('/') + 1).toLowerCase(Locale.ROOT);
            if (!n.endsWith(".rss") || listed.contains(n.substring(0, n.length() - 4))) out.add(e);
        }
        return out;
    }

    /** Copies the wanted files of the remembered folder into `destination`, on a thread of its own. */
    public static void startStaging(final String destination) {
        final GameActivity a = self();
        final String saved = savedFolder();
        stageState = 1;
        stageDone = 0;
        stageTotal = 0;
        stageItem = "";
        stageError = "";
        stageStop = false;
        new Thread(new Runnable() {
            @Override
            public void run() {
                try {
                    ContentResolver resolver = a.getContentResolver();
                    Uri tree = Uri.parse(saved);
                    ArrayList<Entry> files = new ArrayList<>();
                    list(resolver, tree, DocumentsContract.getTreeDocumentId(tree), "", 0, files);
                    files = withoutUnusedSounds(resolver, files);
                    stageTotal = files.size();
                    byte[] buffer = new byte[1 << 16];
                    for (Entry e : files) {
                        if (stageStop) throw new Exception("stopped before the end");
                        stageItem = e.path;
                        File to = new File(destination, e.path);
                        File parent = to.getParentFile();
                        if (parent != null) parent.mkdirs();
                        try (InputStream in = resolver.openInputStream(e.uri); OutputStream out = new FileOutputStream(to)) {
                            if (in == null) throw new Exception("cannot read " + e.path);
                            int n;
                            while ((n = in.read(buffer)) > 0) out.write(buffer, 0, n);
                        }
                        stageDone = stageDone + 1;
                    }
                    stageState = 2;
                } catch (SecurityException e) {
                    stageError = "The folder may not be read any more. Choose it again (Options).";
                    stageState = 3;
                } catch (Exception e) {
                    String m = e.getMessage();
                    stageError = m != null ? m : e.toString();
                    stageState = 3;
                }
            }
        }, "staging").start();
    }

    public static int[] stagingState() {
        return new int[] {stageState, stageDone, stageTotal};
    }

    public static String stagingItem() {
        return stageItem;
    }

    public static String stagingError() {
        return stageError;
    }

    public static void stopStaging() {
        stageStop = true;
    }
}
