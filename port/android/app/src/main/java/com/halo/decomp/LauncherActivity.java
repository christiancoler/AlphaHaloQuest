package com.halo.decomp;

import android.app.Activity;
import android.content.ContentResolver;
import android.content.Intent;
import android.database.Cursor;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

/**
 * Starts the game once its data is in place.
 *
 * The game reads the Xbox game data (the folder holding maps/) from the
 * app's external files directory. If it is missing, this screen can extract
 * it directly from a legally dumped Xbox disc image, import an already
 * extracted folder, or let the player push the folder with adb.
 */
public class LauncherActivity extends Activity {
    private static final int PICK_FOLDER = 1;
    private static final int PICK_IMAGE = 2;

    private static final int XISO_SECTOR_SIZE = 2048;
    private static final int XISO_VOLUME_DESCRIPTOR_OFFSET = 0x10000;
    private static final int XISO_ENTRY_HEADER_SIZE = 14;
    private static final int XISO_DIRECTORY_ATTRIBUTE = 0x10;
    private static final int XISO_MAXIMUM_DIRECTORY_SIZE = 4 << 20;
    private static final int XISO_MAXIMUM_FILES = 256;
    private static final int COPY_BUFFER_SIZE = 1 << 20;
    private static final long[] XISO_PARTITION_OFFSETS = {
        0L, 0x0FD90000L, 0x02080000L, 0x18300000L
    };
    private static final byte[] XISO_VOLUME_MAGIC =
        "MICROSOFT*XBOX*MEDIA".getBytes(StandardCharsets.US_ASCII);

    private File dataRoot;
    private TextView status;
    private ProgressBar progress;
    private Button pick;
    private Button pickImage;
    private boolean autoStartWhenDataAppears;
    private final Handler handler = new Handler(Looper.getMainLooper());

    private static final class Entry {
        final Uri uri;
        final String path;
        final long size;

        Entry(Uri uri, String path, long size) {
            this.uri = uri;
            this.path = path;
            this.size = size;
        }
    }

    private static final class XisoEntry {
        final String name;
        final long sector;
        final long size;

        XisoEntry(String name, long sector, long size) {
            this.name = name;
            this.sector = sector;
            this.size = size;
        }
    }

    private static final class XisoVolume {
        final long partition;
        final long rootSector;
        final long rootSize;

        XisoVolume(long partition, long rootSector, long rootSize) {
            this.partition = partition;
            this.rootSector = rootSector;
            this.rootSize = rootSize;
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        dataRoot = getExternalFilesDir(null);
        // created by the app, so that files pushed into it with adb stay
        // readable (a directory adb creates there belongs to the shell user)
        if (dataRoot != null)
            new File(dataRoot, "maps").mkdirs();
        passOnInvite(getIntent());
        if (haveData()) {
            buildReadyInterface();
            return;
        }
        buildInterface();
    }

    /**
     * An internet play invite link the app was opened with: the game
     * (port/linux/src/p2p.c) picks it up from join_link.txt, whether it is
     * starting now or already running.
     */
    private void passOnInvite(Intent intent) {
        if (intent == null || !Intent.ACTION_VIEW.equals(intent.getAction()) || intent.getData() == null
            || dataRoot == null)
            return;
        try (OutputStream out = new FileOutputStream(new File(dataRoot, "join_link.txt"))) {
            out.write(intent.getData().toString().getBytes("UTF-8"));
        } catch (java.io.IOException e) {
            // the link is lost; the player can copy it instead
        }
    }

    private boolean haveData() {
        return dataRoot != null && new File(dataRoot, "maps/ui.map").isFile();
    }

    private void startGame() {
        Intent game = new Intent(this, HaloActivity.class);
        game.addCategory("org.khronos.openxr.intent.category.IMMERSIVE_HMD");
        game.addCategory("com.oculus.intent.category.VR");
        startActivity(game);
        finish();
    }

    private int dp(float value) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, value,
            getResources().getDisplayMetrics());
    }

    private void buildReadyInterface() {
        autoStartWhenDataAppears = false;
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setGravity(Gravity.CENTER);
        layout.setPadding(dp(48), dp(24), dp(48), dp(24));
        layout.setBackgroundColor(Color.rgb(12, 16, 20));

        TextView title = new TextView(this);
        title.setText("AlphaHaloQuest - build " + BuildConfig.HALO_BUILD_NUMBER);
        title.setTextColor(Color.WHITE);
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 26);
        title.setGravity(Gravity.CENTER);
        layout.addView(title);

        TextView controls = new TextView(this);
        controls.setText("CONTROLS\n\n"
            + "Left stick: move     Right stick: look / snap turn\n"
            + "Right trigger: fire     Left trigger: grenade\n"
            + "A: jump / select       B: melee / back\n"
            + "X: use / reload        Y: change weapon\n"
            + "Right stick click: zoom     Left stick click: crouch\n"
            + "Menu: pause\n\n"
            + "Two-hand support: bring the left hand near the rifle foregrip.\n"
            + "It snaps on automatically and releases when moved away.\n"
            + "Right grip: change grenade type.\n"
            + "Bring the off-hand to your headset to toggle the flashlight.\n\n"
            + "EARLY ALPHA: expect broken controls, visuals, crashes, and regressions.\n"
            + "The game uses the original Xbox Halo CE data already imported on this headset.");
        controls.setTextColor(Color.rgb(220, 225, 230));
        controls.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
        controls.setGravity(Gravity.CENTER);
        controls.setPadding(0, dp(18), 0, dp(18));
        layout.addView(controls);

        Button play = new Button(this);
        play.setText("Start AlphaHaloQuest");
        play.setOnClickListener(v -> startGame());
        layout.addView(play, new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT));

        TextView repairMessage = new TextView(this);
        repairMessage.setText("If a campaign level hangs after the menu closes, repair the imported "
            + "maps from your original disc image. Saves and settings are kept.");
        repairMessage.setTextColor(Color.rgb(190, 195, 200));
        repairMessage.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        repairMessage.setGravity(Gravity.CENTER);
        repairMessage.setPadding(0, dp(18), 0, dp(8));
        layout.addView(repairMessage);

        pickImage = new Button(this);
        pickImage.setText("Repair from Xbox disc image");
        pickImage.setOnClickListener(v -> chooseDiscImage());
        layout.addView(pickImage);

        pick = new Button(this);
        pick.setText("Repair from extracted maps folder");
        pick.setOnClickListener(v -> chooseMapsFolder());
        layout.addView(pick);

        Button cleanStart = new Button(this);
        cleanStart.setText("Back up and reset settings + saves");
        cleanStart.setOnClickListener(v -> cleanStart());
        layout.addView(cleanStart);

        progress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progress.setMax(1000);
        progress.setVisibility(View.GONE);
        layout.addView(progress, new LinearLayout.LayoutParams(dp(480),
            LinearLayout.LayoutParams.WRAP_CONTENT));

        status = new TextView(this);
        status.setTextColor(Color.rgb(160, 200, 160));
        status.setGravity(Gravity.CENTER);
        status.setPadding(0, dp(8), 0, 0);
        layout.addView(status);

        setContentView(layout);
        play.requestFocus();
    }

    private void buildInterface() {
        autoStartWhenDataAppears = true;
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setGravity(Gravity.CENTER);
        layout.setPadding(dp(48), dp(24), dp(48), dp(24));
        layout.setBackgroundColor(Color.rgb(12, 16, 20));

        TextView title = new TextView(this);
        title.setText("Halo needs its game data");
        title.setTextColor(Color.WHITE);
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 24);
        title.setGravity(Gravity.CENTER);
        layout.addView(title);

        TextView message = new TextView(this);
        message.setText("Choose a legally dumped original-Xbox Halo: Combat Evolved disc image "
            + "(.iso/.xiso), and this app will extract its maps directly. NTSC and PAL retail discs are "
            + "supported. You can also choose an already extracted folder containing maps/ui.map. "
            + "The imported data uses about 1.8 GB.\n\n"
            + "Or copy an extracted folder from a computer:\n"
            + "adb push <folder>/. " + (dataRoot != null ? dataRoot.getAbsolutePath() : "") + "/");
        message.setTextColor(Color.rgb(200, 205, 210));
        message.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        message.setGravity(Gravity.CENTER);
        message.setPadding(0, dp(16), 0, dp(16));
        layout.addView(message);

        pick = new Button(this);
        pick.setText("Choose extracted maps folder");
        pick.setOnClickListener(v -> chooseMapsFolder());
        layout.addView(pick, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT,
            LinearLayout.LayoutParams.WRAP_CONTENT));

        pickImage = new Button(this);
        pickImage.setText("Choose Xbox disc image (.iso/.xiso)");
        pickImage.setOnClickListener(v -> chooseDiscImage());
        LinearLayout.LayoutParams imageLayout = new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        imageLayout.topMargin = dp(8);
        layout.addView(pickImage, imageLayout);

        progress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progress.setMax(1000);
        progress.setVisibility(View.GONE);
        LinearLayout.LayoutParams progressLayout = new LinearLayout.LayoutParams(dp(480),
            LinearLayout.LayoutParams.WRAP_CONTENT);
        progressLayout.topMargin = dp(16);
        layout.addView(progress, progressLayout);

        status = new TextView(this);
        status.setTextColor(Color.rgb(160, 200, 160));
        status.setGravity(Gravity.CENTER);
        status.setPadding(0, dp(8), 0, 0);
        layout.addView(status);

        setContentView(layout);
        pick.requestFocus();
    }

    private void chooseMapsFolder() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        startActivityForResult(intent, PICK_FOLDER);
    }

    private void chooseDiscImage() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        startActivityForResult(intent, PICK_IMAGE);
    }

    private void cleanStart() {
        File backup = new File(dataRoot, "recovery-backup-" + System.currentTimeMillis());
        File config = new File(dataRoot, "config.toml");
        File saves = new File(dataRoot, "save");
        if (!backup.mkdirs()) {
            status.setText("Could not create a recovery backup.");
            return;
        }
        if (config.exists() && !config.renameTo(new File(backup, "config.toml"))) {
            status.setText("Could not back up config.toml; nothing was reset.");
            return;
        }
        if (saves.exists() && !saves.renameTo(new File(backup, "save"))) {
            File oldConfig = new File(backup, "config.toml");
            if (oldConfig.exists())
                oldConfig.renameTo(config);
            status.setText("Could not back up saves; nothing was reset.");
            return;
        }
        new File(dataRoot, "save").mkdirs();
        status.setText("Backup created. Starting with fresh settings and saves...");
        startGame();
    }

    @Override
    protected void onResume() {
        super.onResume();
        // data pushed with adb while this screen was open
        if (autoStartWhenDataAppears && pick != null && pick.isEnabled() && haveData())
            startGame();
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if ((requestCode != PICK_FOLDER && requestCode != PICK_IMAGE) || resultCode != RESULT_OK ||
            data == null || data.getData() == null)
            return;
        Uri tree = data.getData();
        pick.setEnabled(false);
        pickImage.setEnabled(false);
        progress.setVisibility(View.VISIBLE);
        if (requestCode == PICK_IMAGE) {
            status.setText("Reading the Xbox disc image...");
            new Thread(() -> importDiscImage(tree)).start();
        } else {
            status.setText("Looking for the game data...");
            new Thread(() -> importData(tree)).start();
        }
    }

    private void report(String text, int permille) {
        handler.post(() -> {
            status.setText(text);
            if (permille >= 0)
                progress.setProgress(permille);
        });
    }

    private void fail(String text) {
        handler.post(() -> {
            status.setText(text);
            progress.setVisibility(View.GONE);
            pick.setEnabled(true);
            pickImage.setEnabled(true);
            pick.requestFocus();
        });
    }

    /** the children of a document in the picked tree */
    private List<String[]> children(ContentResolver resolver, Uri tree, String documentId) {
        List<String[]> result = new ArrayList<>();
        Uri uri = DocumentsContract.buildChildDocumentsUriUsingTree(tree, documentId);
        String[] columns = {
            DocumentsContract.Document.COLUMN_DOCUMENT_ID,
            DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            DocumentsContract.Document.COLUMN_MIME_TYPE,
            DocumentsContract.Document.COLUMN_SIZE,
        };
        try (Cursor cursor = resolver.query(uri, columns, null, null, null)) {
            while (cursor != null && cursor.moveToNext()) {
                result.add(new String[] {
                    cursor.getString(0), cursor.getString(1), cursor.getString(2),
                    cursor.isNull(3) ? "0" : cursor.getString(3),
                });
            }
        }
        return result;
    }

    private void collect(ContentResolver resolver, Uri tree, String documentId, String path, List<Entry> out) {
        for (String[] child : children(resolver, tree, documentId)) {
            String childPath = path.isEmpty() ? child[1] : path + "/" + child[1];
            if (DocumentsContract.Document.MIME_TYPE_DIR.equals(child[2]))
                collect(resolver, tree, child[0], childPath, out);
            else
                out.add(new Entry(DocumentsContract.buildDocumentUriUsingTree(tree, child[0]), childPath,
                    Long.parseLong(child[3])));
        }
    }

    private static long littleU32(byte[] bytes, int offset) {
        return ((long)bytes[offset] & 0xff) |
            (((long)bytes[offset + 1] & 0xff) << 8) |
            (((long)bytes[offset + 2] & 0xff) << 16) |
            (((long)bytes[offset + 3] & 0xff) << 24);
    }

    private static int littleU16(byte[] bytes, int offset) {
        return (bytes[offset] & 0xff) | ((bytes[offset + 1] & 0xff) << 8);
    }

    private static boolean bytesEqual(byte[] data, int offset, byte[] expected) {
        if (offset < 0 || offset + expected.length > data.length)
            return false;
        for (int index = 0; index < expected.length; index++) {
            if (data[offset + index] != expected[index])
                return false;
        }
        return true;
    }

    private static void readAt(FileChannel channel, long offset, byte[] destination, int size)
        throws java.io.IOException {
        ByteBuffer buffer = ByteBuffer.wrap(destination, 0, size);
        long position = offset;
        while (buffer.hasRemaining()) {
            int count = channel.read(buffer, position);
            if (count < 0)
                throw new java.io.IOException("unexpected end of disc image");
            if (count == 0)
                throw new java.io.IOException("the selected file cannot be read randomly");
            position += count;
        }
    }

    private static XisoVolume findXisoVolume(FileChannel channel) throws java.io.IOException {
        byte[] descriptor = new byte[XISO_SECTOR_SIZE];
        for (long partition : XISO_PARTITION_OFFSETS) {
            try {
                readAt(channel, partition + XISO_VOLUME_DESCRIPTOR_OFFSET, descriptor, descriptor.length);
            } catch (java.io.IOException ignored) {
                continue;
            }
            if (bytesEqual(descriptor, 0, XISO_VOLUME_MAGIC) &&
                bytesEqual(descriptor, 0x7ec, XISO_VOLUME_MAGIC)) {
                return new XisoVolume(partition, littleU32(descriptor, 20),
                    littleU32(descriptor, 24));
            }
        }
        throw new java.io.IOException("This is not an original-Xbox XDVDFS disc image. "
            + "PC Halo discs and PC installation files are not compatible.");
    }

    private static byte[] readXisoDirectory(FileChannel channel, long partition, long sector, long size)
        throws java.io.IOException {
        if (size <= 0 || size > XISO_MAXIMUM_DIRECTORY_SIZE)
            throw new java.io.IOException("The disc image has a damaged directory.");
        byte[] table = new byte[(int)size];
        readAt(channel, partition + sector * XISO_SECTOR_SIZE, table, table.length);
        return table;
    }

    private static void walkXisoDirectory(byte[] table, long offsetUnits, int depth,
        boolean directories, List<XisoEntry> entries, int[] visited) {
        long offsetLong = offsetUnits * 4;
        if (depth > 64 || ++visited[0] > 4096 || offsetLong < 0 ||
            offsetLong + XISO_ENTRY_HEADER_SIZE > table.length)
            return;
        int offset = (int)offsetLong;
        int left = littleU16(table, offset);
        int right = littleU16(table, offset + 2);
        if (left == 0xffff)
            return;
        int nameLength = table[offset + 13] & 0xff;
        if (left != 0)
            walkXisoDirectory(table, left, depth + 1, directories, entries, visited);
        boolean isDirectory = (table[offset + 12] & XISO_DIRECTORY_ATTRIBUTE) != 0;
        if (offset + XISO_ENTRY_HEADER_SIZE + nameLength <= table.length && nameLength > 0 &&
            isDirectory == directories && entries.size() < XISO_MAXIMUM_FILES) {
            String name = new String(table, offset + XISO_ENTRY_HEADER_SIZE, nameLength,
                StandardCharsets.ISO_8859_1);
            if (!name.equals(".") && !name.equals("..") && !name.contains("/") &&
                !name.contains("\\")) {
                entries.add(new XisoEntry(name, littleU32(table, offset + 4),
                    littleU32(table, offset + 8)));
            }
        }
        if (right != 0)
            walkXisoDirectory(table, right, depth + 1, directories, entries, visited);
    }

    private static List<XisoEntry> xisoEntries(byte[] table, boolean directories) {
        List<XisoEntry> result = new ArrayList<>();
        walkXisoDirectory(table, 0, 0, directories, result, new int[] { 0 });
        return result;
    }

    private static XisoEntry findXisoEntry(List<XisoEntry> entries, String name) {
        for (XisoEntry entry : entries) {
            if (entry.name.equalsIgnoreCase(name))
                return entry;
        }
        return null;
    }

    private static void deleteTree(File file) {
        if (file == null || !file.exists())
            return;
        if (file.isDirectory()) {
            File[] children = file.listFiles();
            if (children != null) {
                for (File child : children)
                    deleteTree(child);
            }
        }
        file.delete();
    }

    /** Extracts maps/ directly from a plain XISO or a common full-disc Xbox image. */
    private void importDiscImage(Uri imageUri) {
        File partial = new File(dataRoot, "maps.partial");
        File destination = new File(dataRoot, "maps");
        try (ParcelFileDescriptor descriptor = getContentResolver().openFileDescriptor(imageUri, "r");
             FileInputStream input = descriptor != null ?
                 new FileInputStream(descriptor.getFileDescriptor()) : null) {
            if (descriptor == null || input == null)
                throw new java.io.IOException("The selected disc image could not be opened.");
            FileChannel channel = input.getChannel();
            XisoVolume volume = findXisoVolume(channel);
            byte[] rootTable = readXisoDirectory(channel, volume.partition, volume.rootSector,
                volume.rootSize);
            XisoEntry mapsDirectory = findXisoEntry(xisoEntries(rootTable, true), "maps");
            if (mapsDirectory == null)
                throw new java.io.IOException("The Xbox image has no maps folder; it is not Halo CE.");
            byte[] mapsTable = readXisoDirectory(channel, volume.partition, mapsDirectory.sector,
                mapsDirectory.size);
            List<XisoEntry> maps = xisoEntries(mapsTable, false);
            if (findXisoEntry(maps, "ui.map") == null)
                throw new java.io.IOException("The image's maps folder has no ui.map.");

            long total = 0;
            for (XisoEntry entry : maps)
                total += entry.size;
            if (dataRoot.getUsableSpace() < total + (64L << 20))
                throw new java.io.IOException("Not enough free storage; the maps need about "
                    + ((total + (1L << 20) - 1) >> 20) + " MB.");
            deleteTree(partial);
            if (!partial.mkdirs())
                throw new java.io.IOException("Could not create temporary maps storage.");

            byte[] buffer = new byte[COPY_BUFFER_SIZE];
            long done = 0;
            for (XisoEntry entry : maps) {
                File outputFile = new File(partial, entry.name);
                long sourceOffset = volume.partition + entry.sector * XISO_SECTOR_SIZE;
                long remaining = entry.size;
                try (OutputStream output = new FileOutputStream(outputFile)) {
                    while (remaining > 0) {
                        int count = (int)Math.min(buffer.length, remaining);
                        readAt(channel, sourceOffset, buffer, count);
                        output.write(buffer, 0, count);
                        sourceOffset += count;
                        remaining -= count;
                        done += count;
                        report("Extracting maps/" + entry.name + " (" + (done >> 20) + " of "
                            + (total >> 20) + " MB)", total > 0 ? (int)(done * 1000 / total) : 0);
                    }
                }
            }

            File backup = new File(dataRoot, "maps.backup");
            deleteTree(backup);
            if (destination.exists() && !destination.renameTo(backup))
                throw new java.io.IOException("Could not move the old maps aside.");
            if (!partial.renameTo(destination)) {
                if (backup.exists())
                    backup.renameTo(destination);
                throw new java.io.IOException("Could not finish the maps import.");
            }
            deleteTree(backup);
            handler.post(() -> {
                if (haveData())
                    startGame();
                else
                    fail("Extraction finished but maps/ui.map is missing.");
            });
        } catch (Exception exception) {
            deleteTree(partial);
            fail("Disc extraction failed: " + exception.getMessage());
        }
    }

    private void importData(Uri tree) {
        try {
            ContentResolver resolver = getContentResolver();
            String rootId = DocumentsContract.getTreeDocumentId(tree);
            List<Entry> entries = new ArrayList<>();
            collect(resolver, tree, rootId, "", entries);

            // the picked folder holds maps/, or is maps/ itself
            boolean hasMapsFolder = false, isMapsFolder = false;
            for (Entry entry : entries) {
                if (entry.path.equalsIgnoreCase("maps/ui.map"))
                    hasMapsFolder = true;
                if (entry.path.equalsIgnoreCase("ui.map"))
                    isMapsFolder = true;
            }
            if (!hasMapsFolder && !isMapsFolder) {
                fail("That folder does not contain maps/ui.map. Pick the folder that holds \"maps\".");
                return;
            }
            long total = 0, done = 0;
            for (Entry entry : entries)
                total += entry.size;
            byte[] buffer = new byte[1 << 20];
            for (Entry entry : entries) {
                String path = isMapsFolder ? "maps/" + entry.path : entry.path;
                File destination = new File(dataRoot, path);
                File parent = destination.getParentFile();
                if (parent != null)
                    parent.mkdirs();
                File partial = new File(destination.getPath() + ".partial");
                try (InputStream in = resolver.openInputStream(entry.uri);
                     OutputStream out = new FileOutputStream(partial)) {
                    int count;
                    while ((count = in.read(buffer)) > 0) {
                        out.write(buffer, 0, count);
                        done += count;
                        report("Copying " + path + " (" + (done >> 20) + " of " + (total >> 20) + " MB)",
                            total > 0 ? (int) (done * 1000 / total) : 0);
                    }
                }
                if (!partial.renameTo(destination))
                    throw new java.io.IOException("cannot write " + destination);
            }
            handler.post(() -> {
                if (haveData()) {
                    startGame();
                } else {
                    fail("The copy finished but maps/ui.map is missing.");
                }
            });
        } catch (Exception exception) {
            fail("Copying failed: " + exception.getMessage());
        }
    }
}
