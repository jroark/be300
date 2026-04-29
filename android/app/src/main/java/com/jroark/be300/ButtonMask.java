package com.jroark.be300;

import android.graphics.Bitmap;

import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;

final class ButtonMask {
    private static final int THRESHOLD = 128;
    private static final int MIN_AREA = 500;
    private static final int MAX_COMPONENTS = 16;

    final int width;
    final int height;
    private final byte[] active;
    private final Region dpad;
    private final Region rocket;
    private final Region power;
    private final Region ok;
    private final Region esc;

    static final class Hit {
        final int set1;
        final int set2;

        Hit(int set1, int set2) {
            this.set1 = set1;
            this.set2 = set2;
        }
    }

    private static final class Component {
        int area;
        int minX;
        int minY;
        int maxX;
        int maxY;
        double cx;
        double cy;
    }

    private static final class Region {
        final int minX;
        final int minY;
        final int maxX;
        final int maxY;
        final double cx;
        final double cy;

        Region(Component c) {
            this(c.minX, c.minY, c.maxX, c.maxY, c.cx, c.cy);
        }

        Region(Component c, double cx, double cy) {
            this(c.minX, c.minY, c.maxX, c.maxY, cx, cy);
        }

        Region(int minX, int minY, int maxX, int maxY, double cx, double cy) {
            this.minX = minX;
            this.minY = minY;
            this.maxX = maxX;
            this.maxY = maxY;
            this.cx = cx;
            this.cy = cy;
        }

        boolean contains(int x, int y) {
            return x >= minX && x <= maxX && y >= minY && y <= maxY;
        }

        double distanceSq(int x, int y) {
            double dx = x + 0.5 - cx;
            double dy = y + 0.5 - cy;
            return dx * dx + dy * dy;
        }
    }

    private ButtonMask(int width, int height, byte[] active, Region dpad,
            Region rocket, Region power, Region ok, Region esc) {
        this.width = width;
        this.height = height;
        this.active = active;
        this.dpad = dpad;
        this.rocket = rocket;
        this.power = power;
        this.ok = ok;
        this.esc = esc;
    }

    static ButtonMask fromBitmap(Bitmap bitmap) {
        if (bitmap == null || bitmap.getWidth() <= 0 || bitmap.getHeight() <= 0) {
            return null;
        }

        int width = bitmap.getWidth();
        int height = bitmap.getHeight();
        int[] pixels = new int[width * height];
        byte[] active = new byte[pixels.length];
        bitmap.getPixels(pixels, 0, width, 0, 0, width, height);

        for (int i = 0; i < pixels.length; i++) {
            int p = pixels[i];
            int a = (p >>> 24) & 0xff;
            int r = (p >>> 16) & 0xff;
            int g = (p >>> 8) & 0xff;
            int b = p & 0xff;
            int luma = (r * 77) + (g * 150) + (b * 29);
            active[i] = a >= 16 && (luma >> 8) >= THRESHOLD ? (byte)1 : 0;
        }

        List<Component> components = collectComponents(active, width, height);
        Regions regions = assignRegions(components, active, width);
        if (regions == null) {
            return null;
        }
        return new ButtonMask(width, height, active, regions.dpad,
                regions.rocket, regions.power, regions.ok, regions.esc);
    }

    Hit hit(float xFloat, float yFloat) {
        int x = (int)Math.floor(xFloat);
        int y = (int)Math.floor(yFloat);
        if (x < 0 || y < 0 || x >= width || y >= height) {
            return null;
        }
        if (active[y * width + x] == 0) {
            return null;
        }

        if (dpad.contains(x, y)) {
            return hitDpad(x, y);
        }
        if (rocket.contains(x, y) && ok.contains(x, y)) {
            return chooseSideButton(rocket, ok, x, y,
                    new Hit(0, 0x10), new Hit(0x04, 0));
        }
        if (power.contains(x, y) && esc.contains(x, y)) {
            return chooseSideButton(power, esc, x, y,
                    new Hit(0, 0x80), new Hit(0x08, 0));
        }
        if (rocket.contains(x, y)) {
            return new Hit(0, 0x10);
        }
        if (power.contains(x, y)) {
            return new Hit(0, 0x80);
        }
        if (ok.contains(x, y)) {
            return new Hit(0x04, 0);
        }
        if (esc.contains(x, y)) {
            return new Hit(0x08, 0);
        }
        return null;
    }

    private Hit hitDpad(int x, int y) {
        double rx = (dpad.maxX - dpad.minX + 1) / 2.0;
        double ry = (dpad.maxY - dpad.minY + 1) / 2.0;
        double dx = (x + 0.5 - dpad.cx) / rx;
        double dy = (y + 0.5 - dpad.cy) / ry;

        if ((dx * dx) + (dy * dy) < 0.18) {
            return new Hit(0x04, 0);
        }
        if (Math.abs(dy) >= Math.abs(dx)) {
            return new Hit(dy < 0.0 ? 0x10 : 0x20, 0);
        }
        return new Hit(dx > 0.0 ? 0x40 : 0x80, 0);
    }

    private static Hit chooseSideButton(Region upper, Region lower, int x, int y,
            Hit upperHit, Hit lowerHit) {
        return upper.distanceSq(x, y) <= lower.distanceSq(x, y)
                ? upperHit : lowerHit;
    }

    private static List<Component> collectComponents(byte[] active, int width,
            int height) {
        byte[] seen = new byte[active.length];
        int[] queue = new int[active.length];
        ArrayList<Component> components = new ArrayList<>();

        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                int start = y * width + x;
                if (active[start] == 0 || seen[start] != 0) {
                    continue;
                }

                int head = 0;
                int tail = 0;
                int area = 0;
                int minX = x;
                int minY = y;
                int maxX = x;
                int maxY = y;
                long sumX = 0;
                long sumY = 0;

                seen[start] = 1;
                queue[tail++] = start;

                while (head < tail) {
                    int p = queue[head++];
                    int px = p % width;
                    int py = p / width;
                    int y0 = Math.max(0, py - 1);
                    int y1 = Math.min(height - 1, py + 1);
                    int x0 = Math.max(0, px - 1);
                    int x1 = Math.min(width - 1, px + 1);

                    area++;
                    sumX += px;
                    sumY += py;
                    minX = Math.min(minX, px);
                    minY = Math.min(minY, py);
                    maxX = Math.max(maxX, px);
                    maxY = Math.max(maxY, py);

                    for (int ny = y0; ny <= y1; ny++) {
                        for (int nx = x0; nx <= x1; nx++) {
                            if (nx == px && ny == py) {
                                continue;
                            }
                            int np = ny * width + nx;
                            if (active[np] == 0 || seen[np] != 0) {
                                continue;
                            }
                            seen[np] = 1;
                            queue[tail++] = np;
                        }
                    }
                }

                if (area >= MIN_AREA) {
                    Component component = new Component();
                    component.area = area;
                    component.minX = minX;
                    component.minY = minY;
                    component.maxX = maxX;
                    component.maxY = maxY;
                    component.cx = (double)sumX / area;
                    component.cy = (double)sumY / area;
                    insertComponent(components, component);
                }
            }
        }

        Collections.sort(components, COMPONENT_AREA_DESC);
        return components;
    }

    private static void insertComponent(ArrayList<Component> components,
            Component component) {
        components.add(component);
        Collections.sort(components, COMPONENT_AREA_DESC);
        if (components.size() > MAX_COMPONENTS) {
            components.remove(components.size() - 1);
        }
    }

    private static Regions assignRegions(List<Component> components, byte[] active,
            int width) {
        if (components.size() < 3) {
            return null;
        }

        Component dpad = components.get(0);
        if (components.size() < 5) {
            Component leftSide = null;
            Component rightSide = null;
            for (int i = 1; i < components.size(); i++) {
                Component c = components.get(i);
                if (c.cx < dpad.cx) {
                    if (leftSide == null || c.area > leftSide.area) {
                        leftSide = c;
                    }
                } else if (rightSide == null || c.area > rightSide.area) {
                    rightSide = c;
                }
            }
            if (leftSide == null || rightSide == null) {
                return null;
            }

            Split left = splitSideComponent(active, width, leftSide, true);
            Split right = splitSideComponent(active, width, rightSide, false);
            return new Regions(new Region(dpad), left.upper, right.upper,
                    left.lower, right.lower);
        }

        ArrayList<Component> left = new ArrayList<>();
        ArrayList<Component> right = new ArrayList<>();
        for (int i = 1; i < components.size(); i++) {
            Component c = components.get(i);
            (c.cx < dpad.cx ? left : right).add(c);
        }
        if (left.size() < 2 || right.size() < 2) {
            return null;
        }

        Comparator<Component> byY = Comparator.comparingDouble(c -> c.cy);
        Collections.sort(left, byY);
        Collections.sort(right, byY);
        return new Regions(new Region(dpad), new Region(left.get(0)),
                new Region(right.get(0)), new Region(left.get(left.size() - 1)),
                new Region(right.get(right.size() - 1)));
    }

    private static Split splitSideComponent(byte[] active, int width,
            Component component, boolean leftSide) {
        double boxW = component.maxX - component.minX + 1;
        double boxH = component.maxY - component.minY + 1;
        double upperX = component.minX + (leftSide ? 0.25 : 0.75) * boxW;
        double upperY = component.minY + 0.30 * boxH;
        double lowerX = component.minX + (leftSide ? 0.75 : 0.25) * boxW;
        double lowerY = component.minY + 0.82 * boxH;

        for (int iter = 0; iter < 8; iter++) {
            double upperSumX = 0.0;
            double upperSumY = 0.0;
            double lowerSumX = 0.0;
            double lowerSumY = 0.0;
            int upperCount = 0;
            int lowerCount = 0;

            for (int y = component.minY; y <= component.maxY; y++) {
                for (int x = component.minX; x <= component.maxX; x++) {
                    if (active[y * width + x] == 0) {
                        continue;
                    }
                    double du = sq(x - upperX) + sq(y - upperY);
                    double dl = sq(x - lowerX) + sq(y - lowerY);
                    if (du <= dl) {
                        upperSumX += x;
                        upperSumY += y;
                        upperCount++;
                    } else {
                        lowerSumX += x;
                        lowerSumY += y;
                        lowerCount++;
                    }
                }
            }

            if (upperCount != 0) {
                upperX = upperSumX / upperCount;
                upperY = upperSumY / upperCount;
            }
            if (lowerCount != 0) {
                lowerX = lowerSumX / lowerCount;
                lowerY = lowerSumY / lowerCount;
            }
        }

        return new Split(new Region(component, upperX, upperY),
                new Region(component, lowerX, lowerY));
    }

    private static double sq(double value) {
        return value * value;
    }

    private static final Comparator<Component> COMPONENT_AREA_DESC =
            (a, b) -> Integer.compare(b.area, a.area);

    private static final class Split {
        final Region upper;
        final Region lower;

        Split(Region upper, Region lower) {
            this.upper = upper;
            this.lower = lower;
        }
    }

    private static final class Regions {
        final Region dpad;
        final Region rocket;
        final Region power;
        final Region ok;
        final Region esc;

        Regions(Region dpad, Region rocket, Region power, Region ok,
                Region esc) {
            this.dpad = dpad;
            this.rocket = rocket;
            this.power = power;
            this.ok = ok;
            this.esc = esc;
        }
    }
}
