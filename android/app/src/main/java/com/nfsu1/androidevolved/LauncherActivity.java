package com.nfsu1.androidevolved;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.util.DisplayMetrics;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;
import java.io.File;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Launcher in the style of the MW and Carbon ports: game files and PLAY on the left, options on the
 * right (Vulkan driver, resolution, frame cap, texture filtering, anti-aliasing, widescreen, compatibility).
 * This edition adds importing the game folder from a ZIP and sharing the diagnostics ZIP directly.
 */
public final class LauncherActivity extends Activity {
    private static final int REQUEST_DRIVER_ZIP = 45, REQUEST_DRIVER_PROBE = 46, REQUEST_GAME_ZIP = 47, REQUEST_SAVE_ZIP = 48, REQUEST_MOD_ZIP = 49;
    // Launcher accent: Underground orange/red (the NFSU2 port is green).
    private static final int ACCENT = 0xFFFF7A1A;
    // Graphics values the launcher left in each profile .cfg (GameConfig.applyMaximum): the next launch compares
    // against them to tell a choice made in the game's Display Settings from a profile it has not seen yet.
    private static final String GRAPHICS_SEEN = "nfsu1_graphics";

    private LinearLayout checksList, optionsList, sessionCard;
    private TextView status, sessionLine;
    private Button play, access, importButton;
    private boolean playAfterProbe, checkingUpdates, resumed, gameLaunched;
    private SessionSummary lastSession;
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private final Handler main = new Handler(Looper.getMainLooper());
    private final AtomicBoolean importCancelled = new AtomicBoolean();
    private boolean importing;

    @Override protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY |
                View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_FULLSCREEN |
                View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN);
        setContentView(buildScreen());
        checkUpdates(false);
        String updated = ReleaseUpdates.pendingWhatsNew(this);
        if (updated != null) showWhatsNew(updated);
    }

    /** First launch after an update: the installed version's release notes from GitHub. */
    private void showWhatsNew(String version) {
        String bundled = ReleaseUpdates.bundledNotes(this);
        AlertDialog dialog = new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(getString(R.string.whats_new_title, version))
                .setMessage(bundled != null ? ReleaseUpdates.whatsNewSection(bundled) : getString(R.string.whats_new_loading))
                .setPositiveButton(R.string.ok, (d, which) -> ReleaseUpdates.whatsNewSeen(this))
                .setNeutralButton(R.string.whats_new_github, (d, which) -> {
                    ReleaseUpdates.whatsNewSeen(this);
                    openUrl(Uri.parse(ReleaseUpdates.releaseTagUrl(version)));
                })
                .setOnCancelListener(d -> ReleaseUpdates.whatsNewSeen(this))
                .show();
        // The bundled notes are the build's own; GitHub's copy of the release (when visible) replaces them.
        ReleaseUpdates.releaseNotes(this, version, notes -> {
            if (!dialog.isShowing()) return;
            if (notes != null) dialog.setMessage(ReleaseUpdates.whatsNewSection(notes));
            else if (bundled == null) dialog.setMessage(getString(R.string.whats_new_unavailable));
        });
    }

    @Override protected void onResume() {
        super.onResume();
        resumed = true;
        if (gameLaunched) {
            // Back from the game: keep the log buffer's last minutes before Android prunes them.
            gameLaunched = false;
            worker.execute(() -> {
                try { Diagnostics.captureLogTail(this); } catch (Exception error) { android.util.Log.w("NFSU1", "Log tail: " + error); }
            });
        }
        refresh();
        String crash = Diagnostics.unreportedCrash(this);
        if (crash != null) {
            Diagnostics.acknowledgeCrashes(this);
            new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                    .setTitle(R.string.crash_title).setMessage(getString(R.string.crash_message, crash))
                    .setPositiveButton(R.string.opt_share_diagnostics, (dialog, which) -> prepareReport())
                    .setNegativeButton(R.string.crash_not_now, null).show();
        }
    }

    @Override protected void onPause() {
        resumed = false;
        super.onPause();
    }

    // ---- Screen ------------------------------------------------------------------------------------------

    private View buildScreen() {
        FrameLayout screen = new FrameLayout(this);
        screen.setBackground(new GradientDrawable(GradientDrawable.Orientation.TL_BR,
                new int[] {0xFF0E0604, 0xFF2E0B05, 0xFF0E0604}));
        LinearLayout columns = new LinearLayout(this);
        columns.setPadding(dp(24), dp(10), dp(24), dp(10));
        screen.addView(columns, new FrameLayout.LayoutParams(-1, -1));

        // Left: logo, game files and PLAY.
        LinearLayout left = new LinearLayout(this);
        left.setOrientation(LinearLayout.VERTICAL);
        left.setGravity(Gravity.CENTER_HORIZONTAL);
        ScrollView leftScroll = new ScrollView(this);
        leftScroll.setFillViewport(true);
        leftScroll.addView(left);
        columns.addView(leftScroll, new LinearLayout.LayoutParams(0, -1, 1.1f));

        LinearLayout header = new LinearLayout(this);
        header.setGravity(Gravity.CENTER_VERTICAL);
        ImageView logo = new ImageView(this);
        logo.setImageResource(R.drawable.ic_logo);
        logo.setAdjustViewBounds(true);
        header.addView(logo, new LinearLayout.LayoutParams(dp(76), dp(76)));
        LinearLayout titles = new LinearLayout(this);
        titles.setOrientation(LinearLayout.VERTICAL);
        titles.setPadding(dp(14), 0, 0, 0);
        titles.addView(label(getString(R.string.launcher_title), 18, Color.WHITE, true));
        titles.addView(label(getString(R.string.launcher_subtitle), 12, 0x99FFFFFF, false));
        header.addView(titles);
        left.addView(header, new LinearLayout.LayoutParams(-1, -2));

        LinearLayout card = card();
        LinearLayout.LayoutParams cardParams = new LinearLayout.LayoutParams(-1, -2);
        cardParams.topMargin = dp(8);
        left.addView(card, cardParams);
        card.addView(label(getString(R.string.game_files_header), 12, ACCENT, true));
        status = label("", 13, 0xDDFFFFFF, false);
        status.setPadding(0, dp(4), 0, dp(6));
        card.addView(status);
        checksList = new LinearLayout(this);
        checksList.setOrientation(LinearLayout.VERTICAL);
        card.addView(checksList);

        // Last session: what the recorder measured while the game ran (GameTelemetry); tap for the details.
        sessionCard = card();
        LinearLayout.LayoutParams sessionParams = new LinearLayout.LayoutParams(-1, -2);
        sessionParams.topMargin = dp(8);
        sessionCard.addView(label(getString(R.string.session_header), 12, ACCENT, true));
        sessionLine = label("", 13, 0xDDFFFFFF, false);
        sessionLine.setPadding(0, dp(4), 0, 0);
        sessionCard.addView(sessionLine);
        sessionCard.setClickable(true);
        sessionCard.setOnClickListener(view -> showSessionDetails());
        sessionCard.setVisibility(View.GONE);
        left.addView(sessionCard, sessionParams);

        play = actionButton(getString(R.string.play), true);
        play.setOnClickListener(view -> play());
        LinearLayout.LayoutParams playParams = new LinearLayout.LayoutParams(-1, dp(54));
        playParams.topMargin = dp(8);
        left.addView(play, playParams);

        access = actionButton(getString(R.string.allow_access), false);
        access.setOnClickListener(view -> requestAccess());
        LinearLayout.LayoutParams accessParams = new LinearLayout.LayoutParams(-1, dp(46));
        accessParams.topMargin = dp(10);
        left.addView(access, accessParams);

        importButton = actionButton(getString(R.string.import_game_files), false);
        importButton.setOnClickListener(view -> importGameFiles());
        LinearLayout.LayoutParams importParams = new LinearLayout.LayoutParams(-1, dp(46));
        importParams.topMargin = dp(10);
        left.addView(importButton, importParams);

        // Right: options.
        LinearLayout right = card();
        LinearLayout.LayoutParams rightParams = new LinearLayout.LayoutParams(0, -1, 1f);
        rightParams.leftMargin = dp(24);
        columns.addView(right, rightParams);
        right.addView(label(getString(R.string.options_header), 12, ACCENT, true));
        TextView note = label(getString(R.string.options_note), 12, 0x88FFFFFF, false);
        note.setPadding(0, dp(2), 0, dp(4));
        right.addView(note);
        ScrollView scroll = new ScrollView(this);
        optionsList = new LinearLayout(this);
        optionsList.setOrientation(LinearLayout.VERTICAL);
        scroll.addView(optionsList);
        right.addView(scroll, new LinearLayout.LayoutParams(-1, 0, 1f));
        return screen;
    }

    private void refresh() {
        boolean allowed = allowed();
        File root = gameRoot();
        checksList.removeAllViews();
        boolean exe = allowed && new File(root, "Speed.exe").isFile();
        addCheck(getString(R.string.check_speed_exe), exe, true);
        StringBuilder missing = new StringBuilder();
        for (String folder : new String[] {"GLOBAL", "TRACKS", "CARS", "FRONTEND", "LANGUAGES"})
            if (!(allowed && new File(root, folder).isDirectory())) missing.append(missing.length() == 0 ? "" : ", ").append(folder);
        addCheck(missing.length() == 0 ? getString(R.string.check_game_folders) : getString(R.string.check_missing_folders, missing),
                missing.length() == 0, true);
        boolean ready = exe && !languages().isEmpty();
        status.setText(!allowed ? R.string.status_allow_files : ready ? R.string.status_ready : R.string.status_copy_files);
        play.setEnabled(ready && !importing);
        play.setAlpha(ready && !importing ? 1f : .45f);
        access.setVisibility(allowed ? View.GONE : View.VISIBLE);
        importButton.setEnabled(allowed && !importing);
        importButton.setAlpha(allowed && !importing ? 1f : .45f);
        refreshOptions();
        refreshSession();
    }

    /** Reads the newest session record off the UI thread and shows its one-line summary. */
    private void refreshSession() {
        worker.execute(() -> {
            SessionSummary summary = null;
            List<File> sessions = GameTelemetry.sessionFiles(this);
            if (!sessions.isEmpty()) {
                try { summary = SessionSummary.parse(Diagnostics.readText(sessions.get(0), 4 * 1024 * 1024), Diagnostics.exitRecords(this), 0); }
                catch (Exception ignored) { }
            }
            SessionSummary found = summary;
            main.post(() -> {
                if (isFinishing() || isDestroyed()) return;
                lastSession = found;
                sessionCard.setVisibility(found == null ? View.GONE : View.VISIBLE);
                if (found != null) sessionLine.setText(found.shortLine() + "  ›");
            });
        });
    }

    private void showSessionDetails() {
        if (lastSession == null) return;
        ScrollView scroll = new ScrollView(this);
        TextView text = label(lastSession.report, 11, 0xDDFFFFFF, false);
        text.setTypeface(Typeface.MONOSPACE);
        text.setPadding(dp(16), dp(8), dp(16), dp(8));
        text.setTextIsSelectable(true);
        scroll.addView(text);
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(getString(R.string.session_details_title, lastSession.session))
                .setView(scroll)
                .setPositiveButton(R.string.opt_share_diagnostics, (dialog, which) -> prepareReport())
                .setNegativeButton(R.string.close, null).show();
    }

    private void refreshOptions() {
        optionsList.removeAllViews();
        GpuDrivers.Driver driver = GpuDrivers.selected(this);
        // Custom drivers (libadrenotools) exist only in the 64-bit build.
        if (android.os.Process.is64Bit())
            optionsList.addView(optionRow(getString(R.string.opt_vulkan_driver),
                    driver == null ? getString(R.string.opt_vulkan_driver_system) : driver.name, this::chooseDriver));
        optionsList.addView(optionRow(getString(R.string.opt_probe_driver), getString(R.string.opt_probe_driver_value), () -> probeDriver(false)));
        optionsList.addView(optionRow(getString(R.string.opt_language), languageLabel(), this::chooseLanguage));
        optionsList.addView(optionRow(getString(R.string.opt_graphics_cache), getString(R.string.opt_graphics_cache_value), () -> GraphicsCache.prompt(this)));
        int garage = GameOptions.garage(this);
        optionsList.addView(optionRow(getString(R.string.opt_garage), garageLabel(garage), this::chooseGarage));
        for (GameOptions.Option option : GameOptions.ALL)
            optionsList.addView(optionRow(option.title(this), option.label(this, GameOptions.get(this, option.key)), () -> chooseOption(option)));
        optionsList.addView(optionRow(getString(R.string.opt_online_server), GameOptions.onlineServer(this), this::chooseOnlineServer));
        optionsList.addView(optionRow(getString(R.string.opt_mods_install), getString(R.string.opt_mods_install_value), this::installModPack));
        int packs = allowed() ? ModPacks.installed(gameRoot()).size() : 0;
        optionsList.addView(optionRow(getString(R.string.opt_mods_installed), getResources().getQuantityString(R.plurals.mods_count, packs, packs), this::showModPacks));
        optionsList.addView(optionRow(getString(R.string.opt_saves_backup), getString(R.string.opt_saves_backup_value), this::backupSaves));
        optionsList.addView(optionRow(getString(R.string.opt_saves_restore), getString(R.string.opt_saves_restore_value), this::restoreSaves));
        optionsList.addView(optionRow(getString(R.string.opt_check_updates),
                checkingUpdates ? getString(R.string.opt_check_updates_checking) : getString(R.string.opt_version, ReleaseUpdates.installed(this)),
                () -> checkUpdates(true)));
        optionsList.addView(optionRow(getString(R.string.opt_share_diagnostics), getString(R.string.opt_share_diagnostics_value), this::prepareReport));
    }

    // ---- Options ---------------------------------------------------------------------------------------

    private List<String[]> languages() {
        List<String[]> found = new ArrayList<>();
        File root = gameRoot();
        for (String[] language : GameOptions.LANGUAGES)
            if (GameOptions.hasLanguage(root, language[2])) found.add(language);
        return found;
    }

    private String languageLabel() {
        String current = GameOptions.language(this, gameRoot());
        for (String[] language : GameOptions.LANGUAGES) if (language[1].equals(current)) return language[0];
        return current;
    }

    private void chooseLanguage() {
        List<String[]> found = allowed() ? languages() : new ArrayList<>();
        if (found.isEmpty()) { message(getString(R.string.opt_language), getString(R.string.language_none)); return; }
        String[] names = new String[found.size()];
        int checked = 0;
        String current = GameOptions.language(this, gameRoot());
        for (int i = 0; i < names.length; i++) {
            names[i] = found.get(i)[0];
            if (found.get(i)[1].equals(current)) checked = i;
        }
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert).setTitle(R.string.opt_language)
                .setSingleChoiceItems(names, checked, (dialog, which) -> {
                    GameOptions.prefs(this).edit().putString(GameOptions.LANGUAGE, found.get(which)[1]).apply();
                    dialog.dismiss(); refreshOptions();
                }).setNegativeButton(R.string.cancel, null).show();
    }

    private java.util.Map<String, String> graphicsSeen() {
        java.util.Map<String, String> seen = new java.util.HashMap<>();
        for (java.util.Map.Entry<String, ?> entry : getSharedPreferences(GRAPHICS_SEEN, 0).getAll().entrySet())
            if (entry.getValue() instanceof String) seen.put(entry.getKey(), (String) entry.getValue());
        return seen;
    }

    /** Stores the graphics fingerprints (synchronously: the game process reads and writes the .cfg files next). */
    private void rememberGraphics(java.util.Map<String, String> seen, boolean replace) {
        android.content.SharedPreferences.Editor edit = getSharedPreferences(GRAPHICS_SEEN, 0).edit();
        if (replace) edit.clear();
        for (java.util.Map.Entry<String, String> entry : seen.entrySet()) edit.putString(entry.getKey(), entry.getValue());
        edit.commit();
    }

    private void chooseOption(GameOptions.Option option) {
        String current = GameOptions.get(this, option.key);
        int checked = 0;
        for (int i = 0; i < option.values.length; i++) if (option.values[i].equals(current)) checked = i;
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert).setTitle(option.title(this))
                .setSingleChoiceItems(option.labels(this), checked, (dialog, which) -> {
                    GameOptions.set(this, option.key, option.values[which]);
                    // Picking Maximum, even when it is already chosen, puts every profile back at the top tier
                    // at the next launch; after that, changes made in the game's Display Settings stay.
                    if (option.key.equals(GameOptions.GAMEGFX) && option.values[which].equals("max"))
                        getSharedPreferences(GRAPHICS_SEEN, 0).edit().clear().commit();
                    dialog.dismiss(); refreshOptions();
                }).setNegativeButton(R.string.cancel, null).show();
    }

    private String garageLabel(int mask) {
        int on = GameOptions.garageCount(mask), all = GameOptions.GARAGE_KEYS.length;
        return on == all ? getString(R.string.garage_all_on, all) : on == 0 ? getString(R.string.garage_all_off)
                : getString(R.string.garage_some_on, on, all);
    }

    /** Garage: one checkbox per customization freedom; takes effect at the next PLAY. */
    private void chooseGarage() {
        String[] labels = new String[GameOptions.GARAGE_KEYS.length];
        boolean[] checked = new boolean[labels.length];
        int mask = GameOptions.garage(this);
        for (int i = 0; i < labels.length; i++) {
            labels[i] = getString(GameOptions.GARAGE_TITLES[i]);
            checked[i] = (mask & (1 << i)) != 0;
        }
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert).setTitle(R.string.opt_garage)
                .setMultiChoiceItems(labels, checked, (dialog, which, isChecked) -> checked[which] = isChecked)
                .setPositiveButton(R.string.ok, (dialog, which) -> {
                    int chosen = 0;
                    for (int i = 0; i < checked.length; i++) if (checked[i]) chosen |= 1 << i;
                    GameOptions.setGarage(this, chosen);
                    refreshOptions();
                })
                .setNeutralButton(R.string.garage_about, (dialog, which) -> message(getString(R.string.opt_garage), getString(R.string.garage_about_text)))
                .setNegativeButton(R.string.cancel, null).show();
    }

    private void chooseOnlineServer() {
        android.widget.EditText input = new android.widget.EditText(this);
        input.setSingleLine(true);
        input.setText(GameOptions.onlineServer(this));
        input.setSelection(input.getText().length());
        input.setInputType(android.text.InputType.TYPE_CLASS_TEXT | android.text.InputType.TYPE_TEXT_VARIATION_URI);
        FrameLayout box = new FrameLayout(this);
        int pad = (int) (20 * getResources().getDisplayMetrics().density);
        box.setPadding(pad, pad / 2, pad, 0);
        box.addView(input);
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert).setTitle(R.string.opt_online_server)
                .setMessage(R.string.online_server_message)
                .setView(box)
                .setPositiveButton(R.string.save, (dialog, which) -> {
                    String value = input.getText().toString().trim();
                    if (!GameOptions.validServer(value)) { message(getString(R.string.opt_online_server), getString(R.string.online_server_invalid, value)); return; }
                    GameOptions.prefs(this).edit().putString(GameOptions.ONLINE_SERVER, value).apply();
                    refreshOptions();
                })
                .setNeutralButton(R.string.restore, (dialog, which) -> {
                    GameOptions.prefs(this).edit().remove(GameOptions.ONLINE_SERVER).apply();
                    refreshOptions();
                })
                .setNegativeButton(R.string.cancel, null).show();
    }

    private void chooseDriver() {
        List<GpuDrivers.Driver> drivers = GpuDrivers.list(this);
        String[] names = new String[drivers.size() + 1];
        names[0] = getString(R.string.driver_system);
        int selected = 0;
        for (int i = 0; i < drivers.size(); ++i) {
            names[i + 1] = drivers.get(i).name;
            if (drivers.get(i).id.equals(GpuDrivers.selectedId(this))) selected = i + 1;
        }
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert).setTitle(R.string.opt_vulkan_driver)
                .setSingleChoiceItems(names, selected, (dialog, which) -> {
                    GpuDrivers.select(this, which == 0 ? "" : drivers.get(which - 1).id);
                    refreshOptions(); dialog.dismiss();
                })
                .setPositiveButton(R.string.driver_import_zip, (dialog, which) -> startActivityForResult(
                        new Intent(Intent.ACTION_OPEN_DOCUMENT).setType("*/*").addCategory(Intent.CATEGORY_OPENABLE),
                        REQUEST_DRIVER_ZIP))
                .setNeutralButton(R.string.driver_remove_selected, (dialog, which) -> {
                    GpuDrivers.Driver driver = GpuDrivers.selected(this);
                    if (driver == null) return;
                    try { GpuDrivers.remove(this, driver); refreshOptions(); }
                    catch (java.io.IOException error) { message(getString(R.string.opt_vulkan_driver), error.getMessage()); }
                })
                .setNegativeButton(R.string.back, null).show();
    }

    private void probeDriver(boolean thenPlay) {
        playAfterProbe = thenPlay;
        play.setEnabled(false);
        startActivityForResult(new Intent(this, GpuProbeActivity.class), REQUEST_DRIVER_PROBE);
    }

    private void showDriverReport(String report) {
        play.setEnabled(true);
        try {
            org.json.JSONObject gpu = new org.json.JSONObject(report);
            boolean failed = gpu.optBoolean("driverLoadFailed") || gpu.has("probeError");
            if (playAfterProbe && !failed && gpu.optBoolean("compatible")) { playAfterProbe = false; launch(); return; }
            // The player pressed PLAY: a driver that loads but lacks a feature the
            // probe checks may still run the game (issue #1107), so offer it.
            boolean wantedToPlay = playAfterProbe && !failed;
            playAfterProbe = false;
            String text;
            if (failed) {
                text = getString(R.string.probe_failed, gpu.optString("probeError", getString(R.string.probe_default_error)));
            } else {
                StringBuilder lines = new StringBuilder();
                lines.append(getString(R.string.probe_gpu, gpu.optString("gpu"))).append('\n')
                     .append(getString(R.string.probe_vulkan, gpu.optString("vulkan"))).append('\n')
                     .append(getString(R.string.probe_driver, gpu.optString("driverName", "—"), gpu.optString("driverInfo", ""))).append('\n')
                     .append(getString(R.string.probe_bc_textures, getString(gpu.optBoolean("bcTextures") ? R.string.probe_bc_gpu : R.string.probe_bc_cpu)));
                if (!gpu.optString("vertexFormatsMissing").isEmpty())
                    lines.append('\n').append(getString(R.string.probe_vertex_missing, gpu.optString("vertexFormatsMissing")));
                lines.append('\n').append(gpu.optBoolean("compatible") ? getString(R.string.probe_compatible)
                        : getString(R.string.probe_missing, String.valueOf(gpu.optJSONArray("missing"))));
                text = lines.toString();
            }
            new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                    .setTitle(R.string.probe_title).setMessage(text).setPositiveButton(R.string.ok, null)
                    .setNeutralButton(R.string.probe_use_system, (dialog, which) -> { GpuDrivers.select(this, ""); refreshOptions(); })
                    .setNegativeButton(wantedToPlay ? getString(R.string.probe_play_anyway) : null, wantedToPlay ? (dialog, which) -> launch() : null)
                    .show();
        } catch (Exception error) { message(getString(R.string.probe_title), getString(R.string.probe_invalid, error.getMessage())); }
    }

    @Override protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQUEST_DRIVER_PROBE) {
            showDriverReport(resultCode == RESULT_OK && data != null ? data.getStringExtra("gpu_report")
                    : "{\"probeError\":\"" + getString(R.string.probe_cancelled) + "\",\"driverLoadFailed\":true}");
        } else if (requestCode == REQUEST_DRIVER_ZIP && resultCode == RESULT_OK && data != null && data.getData() != null) {
            Uri zip = data.getData();
            status.setText(R.string.driver_importing);
            worker.execute(() -> {
                try {
                    GpuDrivers.Driver driver = GpuDrivers.importZip(this, zip);
                    main.post(() -> {
                        GpuDrivers.select(this, driver.id); refresh();
                        status.setText(getString(R.string.driver_imported, driver.name));
                    });
                } catch (Exception error) {
                    main.post(() -> { refresh(); message(getString(R.string.opt_vulkan_driver), getString(R.string.driver_import_failed, error.getMessage())); });
                }
            });
        } else if (requestCode == REQUEST_GAME_ZIP && resultCode == RESULT_OK && data != null && data.getData() != null) {
            runImport(data.getData());
        } else if (requestCode == REQUEST_SAVE_ZIP && resultCode == RESULT_OK && data != null && data.getData() != null) {
            runSaveRestore(data.getData());
        } else if (requestCode == REQUEST_MOD_ZIP && resultCode == RESULT_OK && data != null && data.getData() != null) {
            runModInstall(data.getData());
        }
    }

    // ---- Mod packs ---------------------------------------------------------------------------------------

    private void installModPack() {
        if (!allowed()) { message(getString(R.string.mods_title), getString(R.string.import_err_access)); return; }
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(R.string.mods_title).setMessage(R.string.mods_install_message)
                .setPositiveButton(R.string.import_choose, (dialog, which) -> startActivityForResult(
                        new Intent(Intent.ACTION_OPEN_DOCUMENT).setType("*/*")
                                .putExtra(Intent.EXTRA_MIME_TYPES, new String[] {"application/zip", "application/x-zip-compressed", "application/octet-stream"})
                                .addCategory(Intent.CATEGORY_OPENABLE), REQUEST_MOD_ZIP))
                .setNegativeButton(R.string.cancel, null).show();
    }

    private String displayName(Uri uri) {
        try (android.database.Cursor cursor = getContentResolver().query(uri, new String[] {android.provider.OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) return cursor.getString(0);
        } catch (RuntimeException ignored) { }
        return uri.getLastPathSegment();
    }

    private void runModInstall(Uri zip) {
        killGameProcess();
        importing = true;
        refresh();
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        TextView text = label(getString(R.string.mods_installing, 0L), 14, Color.WHITE, false);
        text.setPadding(dp(24), dp(8), dp(24), dp(8));
        AlertDialog dialog = new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(R.string.mods_title).setView(text).setCancelable(false).create();
        dialog.show();
        String name = displayName(zip);
        final long[] lastShown = {0};
        worker.execute(() -> {
            try (java.io.InputStream input = getContentResolver().openInputStream(zip)) {
                if (input == null) throw new java.io.IOException(getString(R.string.saves_err_open));
                ModPacks.Pack pack = ModPacks.install(input, gameRoot(), name, (bytes, entry) -> {
                    long now = System.currentTimeMillis();
                    if (now - lastShown[0] < 200) return;
                    lastShown[0] = now;
                    main.post(() -> text.setText(getString(R.string.mods_installing, bytes >> 20)));
                });
                main.post(() -> {
                    finishImport(dialog);
                    String notes = pack.notes.isEmpty() ? "" : "\n\n" + pack.notes;
                    message(getString(R.string.mods_title), getString(R.string.mods_installed_done, pack.name, pack.files.size(), pack.replaced.size()) + notes);
                });
            } catch (Exception error) {
                main.post(() -> { finishImport(dialog); message(getString(R.string.mods_title), getString(R.string.mods_failed, error.getMessage())); });
            }
        });
    }

    private void showModPacks() {
        if (!allowed()) { message(getString(R.string.mods_title), getString(R.string.import_err_access)); return; }
        List<ModPacks.Pack> packs = ModPacks.installed(gameRoot());
        if (packs.isEmpty()) { message(getString(R.string.opt_mods_installed), getString(R.string.mods_none)); return; }
        String[] lines = new String[packs.size()];
        for (int i = 0; i < packs.size(); i++) {
            ModPacks.Pack pack = packs.get(i);
            lines[i] = (i + 1) + ". " + pack.name + (pack.version.isEmpty() ? "" : " " + pack.version) + "\n"
                    + getString(R.string.mods_pack_line, pack.files.size(), java.text.DateFormat.getDateTimeInstance(java.text.DateFormat.SHORT, java.text.DateFormat.SHORT).format(new java.util.Date(pack.installedAt)));
        }
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(R.string.opt_mods_installed)
                .setItems(lines, (dialog, which) -> confirmModRemoval(packs, which))
                .setNegativeButton(R.string.close, null).show();
    }

    private void confirmModRemoval(List<ModPacks.Pack> packs, int index) {
        ModPacks.Pack pack = packs.get(index);
        int later = packs.size() - index - 1;
        String message = later == 0 ? getString(R.string.mods_remove_message, pack.name)
                : getString(R.string.mods_remove_message_stack, pack.name, later);
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(R.string.mods_remove_title).setMessage(message)
                .setPositiveButton(R.string.mods_remove_button, (dialog, which) -> {
                    killGameProcess();
                    status.setText(R.string.mods_removing);
                    worker.execute(() -> {
                        try {
                            List<ModPacks.Pack> removed = ModPacks.uninstall(gameRoot(), pack.id);
                            main.post(() -> { refresh(); message(getString(R.string.mods_title), getString(R.string.mods_removed_done, removed.size())); });
                        } catch (Exception error) {
                            main.post(() -> { refresh(); message(getString(R.string.mods_title), getString(R.string.mods_failed, error.getMessage())); });
                        }
                    });
                })
                .setNegativeButton(R.string.cancel, null).show();
    }

    // ---- Save backup -----------------------------------------------------------------------------------

    private void backupSaves() {
        if (!allowed()) { message(getString(R.string.saves_title), getString(R.string.import_err_access)); return; }
        status.setText(R.string.saves_working);
        worker.execute(() -> {
            try {
                SaveBackup.Result result = SaveBackup.backup(this, gameRoot());
                main.post(() -> {
                    refresh();
                    new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                            .setTitle(R.string.saves_title)
                            .setMessage(getString(R.string.saves_backup_done, result.files, result.location))
                            .setPositiveButton(R.string.share, (dialog, which) -> startActivity(Intent.createChooser(
                                    new Intent(Intent.ACTION_SEND).setType("application/zip").putExtra(Intent.EXTRA_STREAM, result.uri)
                                            .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION), getString(R.string.saves_share_chooser))))
                            .setNegativeButton(R.string.close, null).show();
                });
            } catch (Exception error) {
                main.post(() -> { refresh(); message(getString(R.string.saves_title), getString(R.string.saves_failed, error.getMessage())); });
            }
        });
    }

    private void restoreSaves() {
        if (!allowed()) { message(getString(R.string.saves_title), getString(R.string.import_err_access)); return; }
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(R.string.saves_title).setMessage(R.string.saves_restore_message)
                .setPositiveButton(R.string.import_choose, (dialog, which) -> startActivityForResult(
                        new Intent(Intent.ACTION_OPEN_DOCUMENT).setType("*/*")
                                .putExtra(Intent.EXTRA_MIME_TYPES, new String[] {"application/zip", "application/x-zip-compressed", "application/octet-stream"})
                                .addCategory(Intent.CATEGORY_OPENABLE), REQUEST_SAVE_ZIP))
                .setNegativeButton(R.string.cancel, null).show();
    }

    private void runSaveRestore(Uri zip) {
        killGameProcess();
        status.setText(R.string.saves_working);
        worker.execute(() -> {
            try (java.io.InputStream input = getContentResolver().openInputStream(zip)) {
                if (input == null) throw new java.io.IOException(getString(R.string.saves_err_open));
                SaveBackup.Result result = SaveBackup.restore(this, input, gameRoot());
                // The restored profiles carry the player's own graphics choices: record them as seen so
                // "Game graphics: Maximum" keeps them instead of treating the profiles as new.
                try { rememberGraphics(GameConfig.observe(gameRoot()), false); }
                catch (java.io.IOException e) { android.util.Log.w("NFSU1", "Game graphics: restored profiles not recorded: " + e); }
                main.post(() -> { refresh(); message(getString(R.string.saves_title), getString(R.string.saves_restore_done, result.files)); });
            } catch (Exception error) {
                main.post(() -> { refresh(); message(getString(R.string.saves_title), getString(R.string.saves_failed, error.getMessage())); });
            }
        });
    }

    // ---- Game files import -----------------------------------------------------------------------------

    private void importGameFiles() {
        if (!allowed()) { message(getString(R.string.import_title), getString(R.string.import_err_access)); return; }
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(R.string.import_title).setMessage(R.string.import_message)
                .setPositiveButton(R.string.import_choose, (dialog, which) -> startActivityForResult(
                        new Intent(Intent.ACTION_OPEN_DOCUMENT).setType("*/*")
                                .putExtra(Intent.EXTRA_MIME_TYPES, new String[] {"application/zip", "application/x-zip-compressed", "application/octet-stream"})
                                .addCategory(Intent.CATEGORY_OPENABLE), REQUEST_GAME_ZIP))
                .setNegativeButton(R.string.cancel, null).show();
    }

    private void runImport(Uri zip) {
        importing = true;
        importCancelled.set(false);
        refresh();
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(24), dp(8), dp(24), dp(8));
        TextView text = label(getString(R.string.import_scanning), 14, Color.WHITE, false);
        ProgressBar bar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        bar.setMax(1000);
        bar.setIndeterminate(true);
        box.addView(text);
        box.addView(bar, new LinearLayout.LayoutParams(-1, -2));
        AlertDialog dialog = new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(R.string.import_title).setView(box).setCancelable(false)
                .setNegativeButton(R.string.cancel, null).create();
        dialog.show();
        dialog.getButton(AlertDialog.BUTTON_NEGATIVE).setOnClickListener(v -> new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(R.string.import_cancel_title).setMessage(R.string.import_cancel_message)
                .setPositiveButton(R.string.cancel, (d, which) -> importCancelled.set(true))
                .setNegativeButton(R.string.import_keep_going, null).show());
        final long[] lastShown = {0};
        worker.execute(() -> {
            try {
                GameImport.Result result = GameImport.run(this, zip, gameRoot(), importCancelled, (done, total, entry) -> {
                    long now = System.currentTimeMillis();
                    if (now - lastShown[0] < 150 && done != total) return;
                    lastShown[0] = now;
                    main.post(() -> {
                        if (total > 0) {
                            bar.setIndeterminate(false);
                            bar.setProgress((int) Math.min(1000, done * 1000 / total));
                            text.setText(getString(R.string.import_progress, done / 1048576, total / 1048576));
                        } else {
                            text.setText(getString(R.string.import_progress, done / 1048576, 0));
                        }
                    });
                });
                main.post(() -> {
                    finishImport(dialog);
                    message(getString(R.string.import_title), getString(R.string.import_done, result.files, result.bytes / 1048576));
                });
            } catch (GameImport.Failure failure) {
                main.post(() -> {
                    finishImport(dialog);
                    message(getString(R.string.import_title), failure.messageRes == R.string.import_cancelled
                            ? failure.message(this) : getString(R.string.import_failed, failure.message(this)));
                });
            } catch (Exception error) {
                main.post(() -> { finishImport(dialog); message(getString(R.string.import_title), getString(R.string.import_failed, String.valueOf(error))); });
            }
        });
    }

    private void finishImport(AlertDialog dialog) {
        importing = false;
        getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (dialog.isShowing()) dialog.dismiss();
        if (!isFinishing() && !isDestroyed()) refresh();
    }

    // ---- Updates and reports ---------------------------------------------------------------------------

    private void checkUpdates(boolean manual) {
        if (checkingUpdates) return;
        checkingUpdates = true;
        if (optionsList != null) refreshOptions();
        ReleaseUpdates.check(this, manual, result -> {
            checkingUpdates = false;
            if (isFinishing() || isDestroyed()) return;
            refreshOptions();
            if (result.error != null) { if (manual) message(getString(R.string.updates_title), result.error); return; }
            if (result.hidden) {
                if (manual) new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                        .setTitle(R.string.updates_title)
                        .setMessage(getString(R.string.updates_private, ReleaseUpdates.installed(this)))
                        .setPositiveButton(R.string.whats_new_github, (dialog, which) -> openUrl(Uri.parse(result.url)))
                        .setNegativeButton(R.string.close, null).show();
                return;
            }
            if (!ReleaseVersion.newer(result.tag, ReleaseUpdates.installed(this))) {
                if (manual) message(getString(R.string.updates_title), getString(R.string.updates_up_to_date, ReleaseUpdates.installed(this)));
                return;
            }
            android.content.SharedPreferences updates = ReleaseUpdates.prefs(this);
            if (!manual && result.tag.equals(updates.getString("notified_tag", ""))) return;
            updates.edit().putString("notified_tag", result.tag).apply();
            new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                    .setTitle(getString(R.string.update_available_title, result.tag))
                    .setMessage(R.string.update_available_message)
                    .setPositiveButton(R.string.update_now, (dialog, which) -> openUrl(Uri.parse(result.url)))
                    .setNegativeButton(R.string.update_later, null).show();
        });
    }

    /**
     * Builds the report ZIP (this app's log, device, options, graphics cache; never game files or saves),
     * saves it to Download/NFSU1 and opens the system share sheet so it can go to Drive, email or a chat.
     */
    private void prepareReport() {
        status.setText(R.string.report_preparing);
        worker.execute(() -> {
            try {
                File report = Diagnostics.createReport(this);
                Diagnostics.Saved saved = Diagnostics.saveToDownloads(this, report);
                main.post(() -> {
                    refresh();
                    new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                            .setTitle(R.string.opt_share_diagnostics)
                            .setMessage(getString(R.string.report_message, saved.location))
                            .setPositiveButton(R.string.share, (dialog, which) -> startActivity(Diagnostics.shareIntent(this, saved.uri)))
                            .setNegativeButton(R.string.close, null).show();
                });
            } catch (Exception error) {
                main.post(() -> { refresh(); message(getString(R.string.opt_share_diagnostics), getString(R.string.report_failed, error.getMessage())); });
            }
        });
    }

    private void openUrl(Uri uri) {
        try { startActivity(new Intent(Intent.ACTION_VIEW, uri)); }
        catch (android.content.ActivityNotFoundException error) { message(getString(R.string.github), getString(R.string.no_browser, uri)); }
    }

    // ---- Launch ----------------------------------------------------------------------------------------

    private void play() {
        if (android.os.Process.is64Bit() && GpuDrivers.selected(this) != null) probeDriver(true);
        else launch();
    }

    private void killGameProcess() {
        android.app.ActivityManager manager = (android.app.ActivityManager) getSystemService(ACTIVITY_SERVICE);
        List<android.app.ActivityManager.RunningAppProcessInfo> processes = manager == null ? null : manager.getRunningAppProcesses();
        if (processes != null) for (android.app.ActivityManager.RunningAppProcessInfo process : processes)
            if (process.uid == android.os.Process.myUid() && process.processName.equals(getPackageName() + ":game"))
                android.os.Process.killProcess(process.pid);
    }

    private void launch() {
        // Each launch uses a fresh guest machine in the dedicated :game process.
        killGameProcess();
        // The bundled shader caches go in before DXVK opens the cache directory (ShaderCacheSeed).
        android.util.Log.i("NFSU1", "Graphics cache: " + ShaderCacheSeed.seed(this));
        if (GameOptions.get(this, GameOptions.GAMEGFX).equals("max")) {
            try {
                GameConfig.Outcome outcome = GameConfig.applyMaximum(gameRoot(), graphicsSeen());
                rememberGraphics(outcome.seen, true);
                android.util.Log.i("NFSU1", "Game graphics: maximum tier written to " + outcome.raised.size() + " new profile(s); "
                        + outcome.kept.size() + " profile(s) keep the choices made in the game since the last launch");
            } catch (java.io.IOException e) {
                android.util.Log.w("NFSU1", "Game graphics: profile .cfg not updated: " + e);
            }
        } else {
            // Nothing is written in this mode; choosing Maximum again raises every profile once.
            getSharedPreferences(GRAPHICS_SEEN, 0).edit().clear().commit();
        }
        gameLaunched = true;
        DisplayMetrics metrics = new DisplayMetrics();
        getWindowManager().getDefaultDisplay().getRealMetrics(metrics);
        int[] size = GameOptions.renderSize(this, Math.max(metrics.widthPixels, metrics.heightPixels),
                Math.min(metrics.widthPixels, metrics.heightPixels));
        String[] loader = GpuDrivers.loaderArguments(this);
        startActivity(new Intent(this, GameActivity.class)
                .putExtra("language", GameOptions.language(this, gameRoot()))
                .putExtra("renderWidth", size[0]).putExtra("renderHeight", size[1])
                .putExtra("frameCap", GameOptions.intValue(this, GameOptions.FPS))
                .putExtra("anisotropy", GameOptions.intValue(this, GameOptions.ANISOTROPY))
                .putExtra("antialias", GameOptions.intValue(this, GameOptions.ANTIALIAS))
                .putExtra("widescreen", GameOptions.get(this, GameOptions.WIDESCREEN).equals("on"))
                .putExtra("wrongWay", GameOptions.get(this, GameOptions.WRONGWAY).equals("on"))
                .putExtra("freeRoam", GameOptions.get(this, GameOptions.FREEROAM).equals("on"))
                .putExtra("hdReflections", GameOptions.get(this, GameOptions.REFLECTIONS).equals("sharp"))
                .putExtra("garage", GameOptions.garage(this))
                .putExtra("effectStates", GameOptions.get(this, GameOptions.EFFECTS).equals("native"))
                .putExtra("cpuPlacement", GameOptions.get(this, GameOptions.CPU))
                .putExtra("reads", GameOptions.get(this, GameOptions.READS))
                .putExtra("cleanupThread", GameOptions.get(this, GameOptions.CLEANUP).equals("thread"))
                .putExtra("minimapTop", GameOptions.get(this, GameOptions.MINIMAP).equals("top"))
                .putExtra("backBuffers", GameOptions.intValue(this, GameOptions.LATENCY))
                .putExtra("cpuTextures", GameOptions.get(this, GameOptions.TEXTURES).equals("cpu"))
                .putExtra("padJoystick", GameOptions.get(this, GameOptions.GAMEPAD).equals("directinput"))
                .putExtra("onlineServer", GameOptions.onlineServer(this))
                .putExtra("overlay", GameOptions.get(this, GameOptions.OVERLAY))
                .putExtra("gfxlog", GameOptions.get(this, GameOptions.GFXLOG))
                .putExtra("driverLoader", loader));
    }

    // ---- Storage ---------------------------------------------------------------------------------------

    static File gameRoot() { return new File(Environment.getExternalStorageDirectory(), "nfsu1"); }

    private boolean allowed() {
        return Build.VERSION.SDK_INT >= 30 ? Environment.isExternalStorageManager()
            : checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED
                && checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED;
    }

    private void requestAccess() {
        if (Build.VERSION.SDK_INT >= 30)
            startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:" + getPackageName())));
        else requestPermissions(new String[] {Manifest.permission.READ_EXTERNAL_STORAGE, Manifest.permission.WRITE_EXTERNAL_STORAGE}, 1);
    }

    @Override public void onRequestPermissionsResult(int code, String[] permissions, int[] results) {
        super.onRequestPermissionsResult(code, permissions, results);
        refresh();
    }

    @Override protected void onDestroy() {
        importCancelled.set(true);
        worker.shutdown();
        super.onDestroy();
    }

    // ---- Widgets ---------------------------------------------------------------------------------------

    private void addCheck(String name, boolean ok, boolean required) {
        TextView row = label((ok ? "✓  " : required ? "✗  " : "!  ") + name + (ok || required ? "" : getString(R.string.check_optional_suffix)), 13,
                ok ? 0xFF7CD992 : required ? 0xFFFF6B6B : 0xFFFFC857, false);
        row.setPadding(0, dp(2), 0, dp(2));
        checksList.addView(row);
    }

    private View optionRow(String title, String value, Runnable onClick) {
        LinearLayout row = new LinearLayout(this);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(dp(12), dp(8), dp(12), dp(8));
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(0x16FF7A1A);
        bg.setCornerRadius(dp(10));
        row.setBackground(bg);
        row.setClickable(true);
        row.setOnClickListener(v -> onClick.run());
        row.addView(label(title, 14, Color.WHITE, false), new LinearLayout.LayoutParams(0, -2, 1f));
        TextView current = label(value + "  ›", 13, ACCENT, true);
        current.setGravity(Gravity.END);
        row.addView(current, new LinearLayout.LayoutParams(0, -2, 1f));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, -2);
        lp.topMargin = dp(5);
        row.setLayoutParams(lp);
        return row;
    }

    private LinearLayout card() {
        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(dp(16), dp(12), dp(16), dp(12));
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(0xCC1C0D08);
        bg.setCornerRadius(dp(16));
        bg.setStroke(dp(1), 0x44FF7A1A);
        card.setBackground(bg);
        return card;
    }

    private Button actionButton(String text, boolean primary) {
        Button b = new Button(this);
        b.setText(text);
        b.setAllCaps(false);
        b.setTextColor(primary ? 0xFF1F0802 : Color.WHITE);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, primary ? 22 : 15);
        b.setTypeface(Typeface.DEFAULT_BOLD);
        b.setLetterSpacing(primary ? .12f : 0f);
        GradientDrawable bg = primary
                ? new GradientDrawable(GradientDrawable.Orientation.LEFT_RIGHT, new int[] {0xFFFFB13D, 0xFFE3261B})
                : new GradientDrawable();
        if (!primary) { bg.setColor(0x22FFFFFF); bg.setStroke(dp(1), 0x55FFFFFF); }
        bg.setCornerRadius(dp(14));
        b.setBackground(bg);
        b.setStateListAnimator(null);
        return b;
    }

    private TextView label(String text, int sp, int color, boolean bold) {
        TextView t = new TextView(this);
        t.setText(text);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setTextColor(color);
        if (bold) { t.setTypeface(Typeface.DEFAULT_BOLD); t.setLetterSpacing(.06f); }
        return t;
    }

    private void message(String title, String text) {
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(title).setMessage(text).setPositiveButton(R.string.ok, null).show();
    }

    private int dp(int value) { return Math.round(value * getResources().getDisplayMetrics().density); }
}
