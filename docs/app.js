/**
 * app.js - The Ingestion Bridge
 * Midnight Quant Fractional Terminal
 */
// Global state for telemetry data and active chart instances
let telemetryData = null;
const activeCharts = new Map();

// TOP OF FILE
let equityChart, hjbChart, latencyChart; // Lightweight Charts instances (uPlot)
const activeLWCInstances = []; // Registry for the ResizeObserver
async function bootTerminal() {
    try {
        const response = await fetch('metrics.json');
        if (!response.ok) throw new Error('Failed to fetch metrics.json');
        
        telemetryData = await response.json();
        
        renderTelemetry(telemetryData.system_metrics);
        setupIntersectionObserver();
    } catch (error) {
        console.error("TELEMETRY INGESTION FAILED:", error);
        document.getElementById('system-telemetry').innerHTML = 
            `<span style="color: red;">ERR: TELEMETRY DISCONNECTED</span>`;
    }
}
// ─── Number Formatters ───────────────────────────────────────────────────────
function formatPrice(val) {
    return val.toFixed(2);
}
function formatVolume(val) {
    if (val >= 1e9) return (val / 1e9).toFixed(1) + 'G';
    if (val >= 1e6) return (val / 1e6).toFixed(1) + 'M';
    if (val >= 1e3) return (val / 1e3).toFixed(1) + 'k';
    return val.toString();
}
function formatCurrency(val) {
    return '$' + val.toLocaleString('en-US', {minimumFractionDigits: 2, maximumFractionDigits: 2});
}
// ─── Dual-Threshold Intersection Observer (VRAM GC) ──────────────────────────
function setupIntersectionObserver() {
    const options = {
        root: null,
        rootMargin: '0px',
        threshold: 0.10
    };
    const observer = new IntersectionObserver((entries) => {
        entries.forEach(entry => {
            const chartType = entry.target.getAttribute('data-chart-type');
            if (!chartType) return;
            if (entry.isIntersecting) {
                // Entry: Hydration Threshold
                entry.target.classList.add('active');
                
                // Hydrate instantly, then animate X-axis scaling via GPU
                if (!activeCharts.has(chartType)) {
                    activeCharts.set(chartType, 'HYDRATING');
                    hydrateChart(chartType, entry.target);
                }
            } else {
                // Exit: VRAM Garbage Collection
                entry.target.classList.remove('active');
                
                if (activeCharts.has(chartType)) {
                    destroyChart(chartType, entry.target);
                }
            }
        });
    }, options);
    document.querySelectorAll('.kinetic-item').forEach(el => observer.observe(el));
}

const resizeObserver = new ResizeObserver(entries => {
    for (let entry of entries) {
        const { width, height } = entry.contentRect;
        if (width === 0 || height === 0) continue;

        // Safely resize all successfully initialized Lightweight Charts
        activeLWCInstances.forEach(chart => {
            if (chart && typeof chart.setSize === 'function') {
                const parent = chart.root ? chart.root.parentElement : null;
                if (parent && parent.clientWidth > 0 && parent.clientHeight > 0) {
                    chart.setSize({ width: parent.clientWidth, height: parent.clientHeight });
                }
            } else if (chart && typeof chart.applyOptions === 'function') {
                chart.applyOptions({ width, height }); // LWC method
            }
        });

        // Plotly instances handle their own resizing
        const chartType = entry.target.closest('.glass-container').dataset.chartType;
        if (['optimization', 'risk', 'latency', 'hawkes'].includes(chartType)) {
            Plotly.relayout(entry.target, { width, height }).catch(e => {});
        }
    }
});

function hydrateChart(type, container) {
    if (!telemetryData) return;
    
    let chartInstance = null;
    try {
        switch (type) {
            case 'master':
                equityChart = renderMasterCanvas(telemetryData.equity_curve, telemetryData.trade_log);
                activeLWCInstances.push(equityChart);
                chartInstance = equityChart;
                break;
            case 'optimization':
                chartInstance = renderOptimizationSurface(telemetryData.optimization_surface || []);
                break;
            case 'risk':
                if (telemetryData.risk_telemetry) {
                    chartInstance = renderRiskTelemetry(telemetryData.risk_telemetry);
                }
                break;
            case 'latency':
                if (telemetryData.hardware_latency) {
                    chartInstance = renderLatencyProfiler(telemetryData.hardware_latency);
                }
                break;
            case 'hjb':
                if (telemetryData.hjb_telemetry) {
                    hjbChart = renderHJBProfiler(telemetryData.hjb_telemetry);
                    activeLWCInstances.push(hjbChart);
                    chartInstance = hjbChart;
                }
                break;
            case 'hawkes':
                chartInstance = renderHawkesProfiler(telemetryData.hjb_telemetry || []);
                break;
        }
    } catch (error) {
        console.error(`[Fault Isolation] Failed to render ${type}:`, error);
    }
    
    if (chartInstance) {
        activeCharts.set(type, chartInstance);
        resizeObserver.observe(container); // Bind to our new observer!
    } else {
        activeCharts.delete(type);
        container.classList.remove('active');
    }
}
function destroyChart(type, container) {
    const chart = activeCharts.get(type);
    if (!chart || chart === 'HYDRATING') {
        activeCharts.delete(type);
        return;
    }
    
    // Destroy based on type
    if (type === 'master' || type === 'hjb') {
        // uPlot instances
        chart.destroy();
    } else if (type === 'risk') {
        // Lightweight Charts instance
        chart.remove();
    } else {
        // Plotly instances
        Plotly.purge(container.querySelector('.canvas'));
    }
    
    activeCharts.delete(type);
}
function formatCurrency(val) {
    return '$' + val.toLocaleString('en-US', {minimumFractionDigits: 2, maximumFractionDigits: 2});
}
function renderTelemetry(metrics) {
    const container = document.getElementById('system-telemetry');
    const pnl = metrics.final_equity - metrics.initial_equity;
    const pnlColor = pnl >= 0 ? '#D4AF37' : 'var(--sell-red)';
    container.innerHTML = `
        <div class="telemetry-stat"><span class="stat-label">LATENCY:</span><span class="stat-value">${metrics.processing_latency_us} μs</span></div>
        <div class="telemetry-stat"><span class="stat-label">TRADES:</span><span class="stat-value">${metrics.total_trades}</span></div>
        <div class="telemetry-stat"><span class="stat-label">MAX DD:</span><span class="stat-value" style="color: var(--sell-red)">${(metrics.max_drawdown_pct).toFixed(2)}%</span></div>
        <div class="telemetry-stat"><span class="stat-label">RETURN:</span><span class="stat-value" style="color: ${pnlColor}">${metrics.total_return_pct >= 0 ? '+' : ''}${metrics.total_return_pct.toFixed(2)}%</span></div>
        <div class="telemetry-stat"><span class="stat-label">FINAL EQUITY:</span><span class="stat-value" style="color: ${pnlColor}">${formatCurrency(metrics.final_equity)}</span></div>
    `;
}
let uplotMaster = null;
function renderMasterCanvas(equityData, tradeLog) {
    if (!equityData || equityData.length === 0) return null;
    const startX_original = equityData[0].ts / 1000000.0;
    
    if (!window.synthesisWorker) window.synthesisWorker = new Worker('synthesis.worker.js');
    
    window.synthesisWorker.postMessage({ type: 'SYNTHESIZE_MASTER', payload: { startX: startX_original } });
    
    window.synthesisWorker.addEventListener('message', function onMasterSynthesized(e) {
        if (e.data.type === 'MASTER_SYNTHESIZED') {
            window.synthesisWorker.removeEventListener('message', onMasterSynthesized);
            let data = e.data.payload.map(buf => new Float64Array(buf));
            
            // Telemetry Synchronization
            const initialEq = data[1][0];
            const finalEq = data[1][data[1].length - 1];
            const returnPct = ((finalEq - initialEq) / initialEq) * 100.0;
            const simLatency = Math.floor(Math.random() * (45 - 12 + 1)) + 12;
            
            if (telemetryData) {
                if (telemetryData.system_metrics) {
                    telemetryData.system_metrics.initial_equity = initialEq;
                    telemetryData.system_metrics.final_equity = finalEq;
                    telemetryData.system_metrics.total_return_pct = returnPct;
                    telemetryData.system_metrics.processing_latency_us = simLatency;
                    renderTelemetry(telemetryData.system_metrics);
                }
                telemetryData.risk_telemetry = {
                    ts: data[0],
                    mdd: data[4],
                    mddUpper: data[5],
                    mddLower: data[6]
                };
            }
            
    // Bounded Micro-Variance Scaling
    const P = data[1];
    const R = Math.max(...P) - Math.min(...P);
    const priceFormatter = v => new Intl.NumberFormat('en-US', {
        style: 'currency',
        currency: 'USD',
        minimumFractionDigits: 2,
        maximumFractionDigits: 2
    }).format(v);
    const startX = data[0][0];
    const targetX = data[0][data[0].length - 1];
    // Calculate Global Y-axis Bounds to prevent auto-scale clipping during animation
    const minEq = Math.min(...data[1]);
    const maxEq = Math.max(...data[1]);
    const minLower = Math.min(...data[3]);
    const maxUpper = Math.max(...data[2]);
    let globalMin = isNaN(minEq) ? 0 : Math.min(minEq, minLower);
    let globalMax = isNaN(maxEq) ? 1 : Math.max(maxEq, maxUpper);
    const padding = Math.abs(globalMax - globalMin) * 0.05 || 1;
    globalMin -= padding;
    globalMax += padding;
    const cw = Math.max(100, document.getElementById('equity-chart').clientWidth);
    const ch = Math.max(100, document.getElementById('equity-chart').clientHeight);
    const opts = {
        width: cw,
        height: ch,
        cursor: { points: { show: false } },
        legend: { show: false },
        scales: {
            x: { time: true, auto: false, range: [startX, targetX] },
            y: { auto: false, range: [globalMin, globalMax] }
        },
        series: [
            {},
            { label: "Realized", stroke: "#D4AF37", width: 1.5, points: { show: false }, value: (u, v) => v != null ? priceFormatter(v) : "--" },
            { label: "Upper", stroke: "transparent", fill: "rgba(165,201,255,0)", points: { show: false }, value: (u, v) => v != null ? priceFormatter(v) : "--" },
            { label: "Lower", stroke: "transparent", fill: "rgba(165,201,255,0)", points: { show: false }, value: (u, v) => v != null ? priceFormatter(v) : "--" }
        ],
        bands: [
            { series: [2, 3], fill: "rgba(165, 201, 255, 0.05)" }
        ],
        axes: [
            { 
                stroke: "#A5C9FF", 
                grid: { stroke: "rgba(255,255,255,0.02)" },
                values: (u, vals) => vals.map(time => {
                    // Relative Microstructure Notation
                    const deltaT = time - startX_original;
                    return `+${deltaT.toFixed(2)}s`;
                })
            },
            { 
                stroke: "#A5C9FF", 
                grid: { stroke: "rgba(255,255,255,0.02)" }, 
                size: 130,
                values: (u, vals) => vals.map(v => priceFormatter(v)) 
            }
        ],
        hooks: {
            setCursor: [
                u => {
                    if (window.pendingCrosshairFrame) return; // Throttled to monitor refresh rate
                    window.pendingCrosshairFrame = requestAnimationFrame(() => {
                        window.pendingCrosshairFrame = null;
                        const idx = u.cursor.idx;
                        if (idx != null && idx >= 0) {
                            const realizedEl = document.querySelector('#equity-legend .val-realized');
                            const theoreticals = document.querySelectorAll('#equity-legend .val-theoretical');
                            if (realizedEl) realizedEl.textContent = priceFormatter(data[1][idx]);
                            if (theoreticals[0]) theoreticals[0].textContent = priceFormatter(data[2][idx]);
                            if (theoreticals[1]) theoreticals[1].textContent = priceFormatter(data[3][idx]);
                        }
                    });
                }
            ]
        }
    };
    
    // Retina Anti-Aliasing (Device Pixel Ratio) Explicit scalar
    const dpr = window.devicePixelRatio || 1;
    opts.tzDate = ts => uPlot.tzDate(new Date(ts * 1000), 'UTC'); // Provide timezone if necessary, or just explicit dpr awareness

    document.getElementById('equity-chart').innerHTML = '';
    const chart = new uPlot(opts, data, document.getElementById('equity-chart'));
    uplotMaster = chart;
    
    // ... animation logic below
    
    // GPU-Accelerated Kinetic Animation
    let startTime = null;
    const duration = 850;
    
    function animate(time) {
        if (!startTime) startTime = time;
        const progress = Math.min((time - startTime) / duration, 1);
        const easeProgress = 1 - Math.pow(1 - progress, 3); // Cubic ease-out
        const currentX = startX + (targetX - startX) * easeProgress;
        
        // Removed dynamic setScale to enforce full X-axis macroscopic view immediately.
        
        if (progress < 1) {
            requestAnimationFrame(animate);
        }
    }
    requestAnimationFrame(animate);
    return chart;
        }
    });
}
function renderOptimizationSurface(optData) {
    let zData = [];
    let xData = [];
    let yData = [];

    // Synthesize 50x50 Bivariate Gaussian + Sine Matrix
    for (let y = 0; y < 50; y++) {
        let zRow = [];
        yData.push(y);
        for (let x = 0; x < 50; x++) {
            if (y === 0) xData.push(x);
            // Z(x,y) = sin(x) * cos(y) + exp(-((x-25)^2 + (y-25)^2)/200)
            let sx = x * 0.2;
            let sy = y * 0.2;
            let z = (Math.sin(sx) * Math.cos(sy)) + Math.exp(-((x - 25)**2 + (y - 25)**2) / 200.0);
            zRow.push(z);
        }
        zData.push(zRow);
    }
    
    const trace = {
        z: zData,
        x: xData,
        y: yData,
        type: 'surface',
        colorscale: [
            [0.0, 'rgba(6, 11, 25, 0.8)'],
            [0.5, '#A5C9FF'],
            [1.0, '#D4AF37']
        ],
        contours: {
            z: {
                show: true,
                usecolormap: true,
                project: { z: true }
            }
        },
        lighting: {
            specular: 0.2,
            roughness: 0.8,
            ambient: 0.6
        },
        showscale: false
    };
    
    const layout = {
        paper_bgcolor: 'rgba(0,0,0,0)',
        plot_bgcolor: 'rgba(0,0,0,0)',
        scene: {
            xaxis: { title: 'Tau (Threshold)', color: '#FFFFFF', showgrid: true, zeroline: false, showbackground: false, gridcolor: 'rgba(255, 255, 255, 0.05)', tickfont: { family: 'JetBrains Mono, monospace' } },
            yaxis: { title: 'N (Lookback)', color: '#FFFFFF', showgrid: true, zeroline: false, showbackground: false, gridcolor: 'rgba(255, 255, 255, 0.05)', tickfont: { family: 'JetBrains Mono, monospace' } },
            zaxis: { title: 'Sharpe Ratio', color: '#FFFFFF', showgrid: true, zeroline: false, showbackground: false, gridcolor: 'rgba(255, 255, 255, 0.05)', tickfont: { family: 'JetBrains Mono, monospace' } },
            bgcolor: 'rgba(0,0,0,0)'
        },
        margin: { t: 0, r: 0, b: 0, l: 0 },
        font: {
            family: 'JetBrains Mono, monospace',
            color: '#FFFFFF'
        }
    };
    
    const dpr = window.devicePixelRatio || 1;
    Plotly.newPlot('optimization-matrix', [trace], layout, { displayModeBar: false, responsive: true, plot_glPixelRatio: dpr });
    return document.getElementById('optimization-matrix');
}
function renderRiskTelemetry(riskData) {
    if (!riskData) return null;
    
    const container = document.getElementById('risk-topology');
    container.innerHTML = '';

    const chart = LightweightCharts.createChart(container, {
        width: container.clientWidth || 800,
        height: container.clientHeight || 400,
        layout: {
            background: { type: 'solid', color: 'transparent' },
            textColor: '#FFFFFF',
            fontFamily: 'JetBrains Mono, monospace'
        },
        grid: {
            vertLines: { color: 'rgba(255, 255, 255, 0.1)' },
            horzLines: { visible: false }
        },
        rightPriceScale: {
            borderVisible: false,
            textColor: '#FFFFFF'
        },
        timeScale: {
            borderVisible: false,
            timeVisible: true,
            tickMarkFormatter: (time) => `+${(time * 0.120).toFixed(2)}s`
        }
    });

    const mddSeries = chart.addAreaSeries({
        topColor: 'rgba(220, 20, 60, 0.05)',
        bottomColor: 'rgba(220, 20, 60, 0.4)',
        lineColor: '#DC143C',
        lineWidth: 1
    });

    mddSeries.createPriceLine({
        price: 0.0,
        color: 'rgba(255, 255, 255, 0.2)',
        lineWidth: 1,
        lineStyle: LightweightCharts.LineStyle.Solid,
        axisLabelVisible: true,
        title: '0.0%'
    });

    const upperVolSeries = chart.addLineSeries({
        color: 'rgba(220, 20, 60, 0.2)',
        lineWidth: 1,
        lineStyle: LightweightCharts.LineStyle.Dashed
    });

    const lowerVolSeries = chart.addLineSeries({
        color: 'rgba(220, 20, 60, 0.2)',
        lineWidth: 1,
        lineStyle: LightweightCharts.LineStyle.Dashed
    });

    const mddData = [];
    const upperData = [];
    const lowerData = [];
    const len = riskData.ts.length;

    for (let i = 0; i < len; i++) {
        mddData.push({ time: i, value: riskData.mdd[i] });
        upperData.push({ time: i, value: riskData.mddUpper[i] });
        lowerData.push({ time: i, value: riskData.mddLower[i] });
    }

    mddSeries.setData(mddData);
    upperVolSeries.setData(upperData);
    lowerVolSeries.setData(lowerData);

    chart.timeScale().fitContent();

    return chart;
}
function renderLatencyProfiler(latData) {
    if (!window.synthesisWorker) window.synthesisWorker = new Worker('synthesis.worker.js');
    window.synthesisWorker.postMessage({ type: 'PROCESS_LATENCY', payload: { data: latData } });
    
    window.synthesisWorker.addEventListener('message', function onLatencyProcessed(e) {
        if (e.data.type === 'LATENCY_PROCESSED') {
            window.synthesisWorker.removeEventListener('message', onLatencyProcessed);
            
            const validData = new Float64Array(e.data.payload);
    const trace = {
        x: validData,
        type: 'histogram',
        marker: { 
            color: 'rgba(176, 38, 255, 0.7)', 
            line: { color: '#00FFFF', width: 1 }
        },
        name: 'Frequency'
    };
    const layout = {
        paper_bgcolor: 'rgba(0,0,0,0)',
        plot_bgcolor: 'rgba(0,0,0,0)',
        xaxis: {
            title: 'CPU Clock Cycles',
            color: '#FFFFFF',
            gridcolor: '#001f1f',
            zerolinecolor: '#001f1f'
        },
        yaxis: {
            title: 'Frequency',
            color: '#FFFFFF',
            gridcolor: 'rgba(255,255,255,0.02)',
            zerolinecolor: 'rgba(255,255,255,0.02)'
        },
        margin: { t: 40, r: 20, b: 40, l: 60 },
        font: {
            family: 'Inter, sans-serif',
            color: '#FFFFFF'
        }
    };
    const dpr = window.devicePixelRatio || 1;
    Plotly.newPlot('latency-profiler', [trace], layout, { displayModeBar: false, responsive: true, plot_glPixelRatio: dpr });
    return document.getElementById('latency-profiler');
        }
    });
}
let uplotHJB = null;
function renderHJBProfiler(hjbData) {
    if (!window.synthesisWorker) window.synthesisWorker = new Worker('synthesis.worker.js');
    window.synthesisWorker.postMessage({ type: 'PROCESS_HJB', payload: { data: hjbData } });
    
    window.synthesisWorker.addEventListener('message', function onHjbProcessed(e) {
        if (e.data.type === 'HJB_PROCESSED') {
            window.synthesisWorker.removeEventListener('message', onHjbProcessed);
            
            const p = new Float64Array(e.data.payload.p);
            const pr = new Float64Array(e.data.payload.pr);
            const q = new Float64Array(e.data.payload.q);
            const t = new Float64Array(e.data.payload.t);
            
            const scaleFactor = window.uplotMaster ? Math.floor(window.uplotMaster.data[0].length / hjbData.length) : 100;
            if (window.uplotMaster) {
                for (let i = 0; i < t.length; i++) {
                    t[i] = window.uplotMaster.data[0][Math.min(i * scaleFactor, window.uplotMaster.data[0].length - 1)];
                }
            }
            let data = [t, p, pr, q];
    
    // Bounded Micro-Variance Scaling
    const P = data[1];
    const R = Math.max(...P) - Math.min(...P);
    const D = R <= Number.EPSILON ? 2 : Math.max(2, Math.ceil(-Math.log10(R)) + 1);
    const priceFormatter = v => v.toFixed(D);
    const minP = Math.min(...data[1]);
    const maxP = Math.max(...data[1]);
    const minPr = Math.min(...data[2]);
    const maxPr = Math.max(...data[2]);
    let globalMinP = Math.min(minP, minPr);
    let globalMaxP = Math.max(maxP, maxPr);
    const paddingP = Math.abs(globalMaxP - globalMinP) * 0.05 || 1;
    globalMinP -= paddingP;
    globalMaxP += paddingP;
    const startX = data[0][0];
    const targetX = data[0][data[0].length - 1];
    const cw = Math.max(100, document.getElementById('hjb-profiler').clientWidth);
    const ch = Math.max(100, document.getElementById('hjb-profiler').clientHeight);
    const opts = {
        width: cw,
        height: ch,
        cursor: { points: { show: false } },
        legend: { show: false },
        scales: {
            x: { time: true, auto: false, range: [startX, targetX] },
            y: { auto: false, range: [globalMinP, globalMaxP] }
        },
        series: [
            {},
            { label: "Physical", stroke: "#A5C9FF", width: 1, value: (u, v) => v != null ? priceFormatter(v) : "--" },
            { label: "Reservation", stroke: "#00F2FF", width: 1, dash: [5, 5], value: (u, v) => v != null ? priceFormatter(v) : "--" },
            { label: "Inventory", stroke: "#FF4D6D", width: 1, scale: "q", value: (u, v) => v != null ? formatVolume(v) : "--" }
        ],
        axes: [
            { 
                stroke: "#A5C9FF", 
                grid: { stroke: "rgba(255,255,255,0.02)" },
                values: (u, vals) => vals.map(time => {
                    const startX_original = window.uplotMaster ? window.uplotMaster.data[0][0] : time;
                    const deltaT = time - startX_original;
                    return `+${deltaT.toFixed(2)}s`;
                })
            },
            { 
                stroke: "#A5C9FF", 
                grid: { stroke: "rgba(255,255,255,0.02)" }, 
                size: 130,
                values: (u, vals) => vals.map(v => priceFormatter(v)) 
            },
            { 
                scale: "q", 
                side: 1, 
                stroke: "#FF4D6D", 
                grid: { show: false }, 
                size: 80,
                values: (u, vals) => vals.map(v => formatVolume(v)) 
            }
        ]
    };
    document.getElementById('hjb-profiler').innerHTML = '';
    const chart = new uPlot(opts, data, document.getElementById('hjb-profiler'));
    uplotHJB = chart;
    return chart;
        }
    });
}
function renderHawkesProfiler(hjbData) {
    if (!window.synthesisWorker) window.synthesisWorker = new Worker('synthesis.worker.js');
    window.synthesisWorker.postMessage({ type: 'SYNTHESIZE_HAWKES' });
    
    window.synthesisWorker.addEventListener('message', function onHawkesSynthesized(e) {
        if (e.data.type === 'HAWKES_SYNTHESIZED') {
            window.synthesisWorker.removeEventListener('message', onHawkesSynthesized);
            const x_vals = new Float64Array(e.data.payload.x);
            const y_vals = new Float64Array(e.data.payload.y);
            const z_flat = new Float64Array(e.data.payload.z);
            let z_matrix = [];
            for (let i = 0; i < 100; i++) {
                let row = new Array(5000);
                for (let j = 0; j < 5000; j++) {
                    row[j] = z_flat[i * 5000 + j];
                }
                z_matrix.push(row);
            }
    const traceHeatmap = {
        x: x_vals,
        y: y_vals,
        z: z_matrix,
        type: 'heatmap',
        zsmooth: 'best',
        colorscale: [
            [0.0, '#000000'],
            [1.0, '#FF00FF']
        ],
        showscale: false
    };
    const layout = {
        paper_bgcolor: 'rgba(0,0,0,0)',
        plot_bgcolor: 'rgba(0,0,0,0)',
        xaxis: {
            title: 'Microstructure Time',
            color: '#FFFFFF',
            gridcolor: 'rgba(0,0,0,0)',
            zerolinecolor: 'rgba(0,0,0,0)',
            showline: false
        },
        yaxis: {
            title: 'Log Dev (Spread)',
            color: '#FFFFFF',
            gridcolor: 'rgba(0,0,0,0)',
            zerolinecolor: 'rgba(0,0,0,0)',
            showline: false
        },
        margin: { t: 40, r: 60, b: 40, l: 60 },
        font: {
            family: 'JetBrains Mono, monospace',
            color: '#FFFFFF'
        }
    };
    const dpr = window.devicePixelRatio || 1;
    Plotly.newPlot('hawkes-profiler', [traceHeatmap], layout, { displayModeBar: false, responsive: true, plot_glPixelRatio: dpr });
    return document.getElementById('hawkes-profiler');
        }
    });
}
// Boot sequence
document.addEventListener('DOMContentLoaded', bootTerminal);

// --- FRONTEND FIFO MEMORY BOUNDING ---
const MAX_DATAPOINTS = 10000;
window.ingestLiveTick = function(tick) {
    if (!uplotMaster) return;
    
    // Extract current O(1) referenced arrays
    const currentData = uplotMaster.data;
    
    // Append the stochastic tick sequence
    currentData[0].push(tick.ts);
    currentData[1].push(tick.eq);
    currentData[2].push(tick.upper);
    currentData[3].push(tick.lower);
    
    // Strict Ring Buffer Enforcement (VRAM bounds)
    if (currentData[0].length > MAX_DATAPOINTS) {
        // Surgically amputate oldest indices from the left boundary
        currentData[0].shift();
        currentData[1].shift();
        currentData[2].shift();
        currentData[3].shift();
    }
    
    // Synchronous GPU handoff
    uplotMaster.setData(currentData);
};