self.onmessage = function(e) {
    const { type, payload } = e.data;
    
    if (type === 'SYNTHESIZE_MASTER') {
        try {
            const startX_original = payload.startX;
            let rawTs = new Float64Array(5000);
            let rawEq = new Float64Array(5000);
            let rawUpper = new Float64Array(5000);
            let rawLower = new Float64Array(5000);
            let currentEq = 1000000.00;
            const mu = 0.5;
            const sigma = 45.0;
            const dt = 0.120;
            const sqrtDt = Math.sqrt(dt);

            for (let i = 0; i < 5000; i++) {
                rawTs[i] = startX_original + i * dt;
                
                let u = 0, v = 0;
                while (u === 0) u = Math.random();
                while (v === 0) v = Math.random();
                let Z = Math.sqrt(-2.0 * Math.log(u)) * Math.cos(2.0 * Math.PI * v);
                
                currentEq = currentEq + (mu * dt) + (sigma * sqrtDt * Z);
                
                rawEq[i] = currentEq;
                let cone = Math.sqrt(i + 1) * 60.0;
                rawUpper[i] = currentEq + cone + Math.random() * 5.0;
                rawLower[i] = currentEq - cone - Math.random() * 5.0;
            }
            
            self.postMessage({ 
                type: 'MASTER_SYNTHESIZED', 
                payload: [rawTs.buffer, rawEq.buffer, rawUpper.buffer, rawLower.buffer] 
            }, [rawTs.buffer, rawEq.buffer, rawUpper.buffer, rawLower.buffer]);
        } catch (err) {
            console.error("Worker Error in SYNTHESIZE_MASTER:", err);
        }
    }
    
    if (type === 'SYNTHESIZE_HAWKES') {
        try {
            let z_matrix = new Float64Array(500000);
            let x_vals = new Float64Array(5000);
            let y_vals = new Float64Array(100);
            
            for (let x = 0; x < 5000; x++) {
                x_vals[x] = x;
            }
            for (let y = 0; y < 100; y++) {
                let mappedY = -5.0 + (y / 100) * 10.0;
                y_vals[y] = mappedY;
                for (let x = 0; x < 5000; x++) {
                    let intensity = Math.random() * 2.0; 
                    let spread = Math.exp(-0.5 * (mappedY * mappedY) / 0.5); 
                    z_matrix[y * 5000 + x] = intensity * spread;
                }
            }
            
            self.postMessage({ 
                type: 'HAWKES_SYNTHESIZED', 
                payload: { x: x_vals.buffer, y: y_vals.buffer, z: z_matrix.buffer } 
            }, [x_vals.buffer, y_vals.buffer, z_matrix.buffer]);
        } catch (err) {
            console.error("Worker Error in SYNTHESIZE_HAWKES:", err);
        }
    }

    if (type === 'PROCESS_HJB') {
        try {
            const hjbData = payload.data;
            const len = hjbData.length;
            let pBuf = new Float64Array(len);
            let prBuf = new Float64Array(len);
            let qBuf = new Float64Array(len);
            let tBuf = new Float64Array(len);
            const baseTime = Date.now() / 1000;
            for (let i = 0; i < len; i++) {
                pBuf[i] = hjbData[i].p;
                prBuf[i] = hjbData[i].pr;
                qBuf[i] = hjbData[i].q;
                tBuf[i] = baseTime + (i * 12.0);
            }
            self.postMessage({
                type: 'HJB_PROCESSED',
                payload: { p: pBuf.buffer, pr: prBuf.buffer, q: qBuf.buffer, t: tBuf.buffer }
            }, [pBuf.buffer, prBuf.buffer, qBuf.buffer, tBuf.buffer]);
        } catch (err) {
            console.error("Worker Error in PROCESS_HJB:", err);
        }
    }

    if (type === 'PROCESS_LATENCY') {
        try {
            const latData = payload.data;
            const validData = latData.slice(256);
            let latBuf = new Float64Array(validData);
            self.postMessage({
                type: 'LATENCY_PROCESSED',
                payload: latBuf.buffer
            }, [latBuf.buffer]);
        } catch (err) {
            console.error("Worker Error in PROCESS_LATENCY:", err);
        }
    }
};
