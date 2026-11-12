package com.dsrnative.app;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileReader;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;

/** Installs the packages made by make_fixtures.py and compares the result. Run by run.sh. */
public final class SwitchPackageTest {
    static int failures = 0;

    static SwitchPackage.FileSource open(File f) throws Exception {
        FileInputStream in = new FileInputStream(f);
        return new SwitchPackage.FileSource(f.getName(), in.getChannel(), in);
    }

    static void compare(File want, File got, String rel) throws Exception {
        File[] list = want.listFiles();
        for (File w : list) {
            File g = new File(got, w.getName());
            if (w.isDirectory()) {
                compare(w, g, rel + w.getName() + "/");
            } else if (!g.isFile() || !java.util.Arrays.equals(Files.readAllBytes(w.toPath()), Files.readAllBytes(g.toPath()))) {
                System.out.println("  MISMATCH " + rel + w.getName());
                failures++;
            }
        }
    }

    static void copyTree(File from, File to) throws Exception {
        to.mkdirs();
        for (File f : from.listFiles()) {
            if (f.isDirectory()) copyTree(f, new File(to, f.getName()));
            else Files.copy(f.toPath(), new File(to, f.getName()).toPath());
        }
    }

    static void run(String label, File dir, String[] pkgs, String expect, boolean update) throws Exception {
        run(label, dir, pkgs, expect, update, false);
    }

    static void run(String label, File dir, String[] pkgs, String expect, boolean update, boolean overBase) throws Exception {
        SwitchPackage.Keys keys = SwitchPackage.Keys.parse(new FileReader(new File(dir, "prod.keys")));
        File dest = Files.createTempDirectory("dsr").toFile();
        List<SwitchPackage.FileSource> src = new ArrayList<>();
        for (String p : pkgs) src.add(open(new File(dir, p)));
        File installed = null;
        if (overBase) {  // the extracted base game in dest, then the update over it
            copyTree(new File(dir, "expect_base"), dest);
            installed = dest;
        }
        SwitchPackage.Result r = SwitchPackage.install(src, keys, dest, installed, null, -1);
        System.out.println(label + ": build " + r.buildId + " update=" + r.withUpdate + " supported=" + r.supportedBuild
                + " bytes=" + r.bytes + " verified=" + r.blocksVerified + " unverified=" + r.blocksUnverified);
        if (r.withUpdate != update) { System.out.println("  wrong update choice"); failures++; }
        compare(new File(dir, expect), dest, "");
        for (SwitchPackage.FileSource s : src) s.close();
    }

    public static void main(String[] a) throws Exception {
        File dir = new File(a[0]);
        run("nsp", dir, new String[] {"base.nsp"}, "expect_base", false);
        run("xci", dir, new String[] {"game.xci"}, "expect_base", false);
        run("nsp+update", dir, new String[] {"base.nsp", "update.nsp"}, "expect_update", true);
        run("xci+update", dir, new String[] {"update.nsp", "game.xci"}, "expect_update", true);
        run("update over extracted base", dir, new String[] {"update.nsp"}, "expect_update", true, true);
        try {
            run("update only", dir, new String[] {"update.nsp"}, "expect_update", true);
            failures++;
        } catch (SwitchPackage.InstallException e) {
            System.out.println("update only: " + e.title + ": " + e.getMessage());
        }
        System.out.println(failures == 0 ? "ALL OK" : failures + " FAILURES");
        System.exit(failures == 0 ? 0 : 1);
    }
}
