// Connector names (DP-1, HDMI-A-1, ...) for the shell's monitors. Murasaki names its outputs by connector, Mutter's JS
// side only knows monitor indices and rectangles; org.gnome.Mutter.DisplayConfig.GetCurrentState has both.

// logicalMonitors: the unpacked `logical_monitors` of GetCurrentState, each [x, y, scale, transform, primary,
// [[connector, vendor, product, serial], ...], properties]. layoutMonitors: [{x, y, width, height}] from
// Main.layoutManager.monitors. Returns [{name, rect}] for every layout monitor whose connector is known; a logical
// monitor made of several (mirrored) physical ones is named after the first.
export function namedMonitors(logicalMonitors, layoutMonitors) {
    const byPosition = new Map();
    for (const logical of logicalMonitors) {
        const [x, y, , , , physical] = logical;
        if (Array.isArray(physical) && physical.length > 0 && Array.isArray(physical[0]))
            byPosition.set(`${x},${y}`, String(physical[0][0]));
    }
    const result = [];
    for (const monitor of layoutMonitors) {
        const name = byPosition.get(`${monitor.x},${monitor.y}`);
        if (name)
            result.push({name, rect: {x: monitor.x, y: monitor.y, width: monitor.width, height: monitor.height}});
    }
    return result;
}
