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
    private static final int REQUEST_DRIVER_ZIP = 45, REQUEST_DRIVER_PROBE = 46, REQUEST_GAME_ZIP = 47;
    // Launcher accent: Underground orange/red (the NFSU2 port is green).
    private static final int ACCENT = 0xFFFF7A1A;

    private LinearLayout checksList, optionsList;
    private TextView status;
    private Button play, access, importButton;
    private boolean playAfterProbe, checkingUpdates, resumed;
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
        logo.setImageResource(R.drawable.icono);
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
        for (GameOptions.Option option : GameOptions.ALL)
            optionsList.addView(optionRow(option.title(this), option.label(this, GameOptions.get(this, option.key)), () -> chooseOption(option)));
        optionsList.addView(optionRow(getString(R.string.opt_online_server), GameOptions.onlineServer(this), this::chooseOnlineServer));
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

    private void chooseOption(GameOptions.Option option) {
        String current = GameOptions.get(this, option.key);
        int checked = 0;
        for (int i = 0; i < option.values.length; i++) if (option.values[i].equals(current)) checked = i;
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert).setTitle(option.title(this))
                .setSingleChoiceItems(option.labels(this), checked, (dialog, which) -> {
                    GameOptions.set(this, option.key, option.values[which]);
                    dialog.dismiss(); refreshOptions();
                }).setNegativeButton(R.string.cancel, null).show();
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
        }
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

    private void launch() {
        // Each launch uses a fresh guest machine in the dedicated :game process.
        android.app.ActivityManager manager = (android.app.ActivityManager) getSystemService(ACTIVITY_SERVICE);
        List<android.app.ActivityManager.RunningAppProcessInfo> processes = manager.getRunningAppProcesses();
        if (processes != null) for (android.app.ActivityManager.RunningAppProcessInfo process : processes)
            if (process.uid == android.os.Process.myUid() && process.processName.equals(getPackageName() + ":game"))
                android.os.Process.killProcess(process.pid);
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
                .putExtra("minimapTop", GameOptions.get(this, GameOptions.MINIMAP).equals("top"))
                .putExtra("backBuffers", GameOptions.intValue(this, GameOptions.LATENCY))
                .putExtra("cpuTextures", GameOptions.get(this, GameOptions.TEXTURES).equals("cpu"))
                .putExtra("padJoystick", GameOptions.get(this, GameOptions.GAMEPAD).equals("directinput"))
                .putExtra("onlineServer", GameOptions.onlineServer(this))
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
