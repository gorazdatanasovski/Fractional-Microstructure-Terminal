self.onmessage = function(e) {
    const { type, payload } = e.data;
    
    if (type === 'SYNTHESIZE_MASTER') {
        const startX_original = payload.startX;
        let rawTs = [];
        let rawEq = [];
        let rawUpper = [];
        let rawLower = [];
        let currentEq = 1000000.00;
        const mu = 0.5;
        const sigma = 45.0;
        const dt = 0.120;
        const sqrtDt = Math.sqrt(dt);

        for (let i = 0; i < 5000; i++) {
            rawTs.push(startX_original + i * dt);
            
            let u = 0, v = 0;
            while (u === 0) u = Math.random();
            while (v === 0) v = Math.random();
            let Z = Math.sqrt(-2.0 * Math.log(u)) * Math.cos(2.0 * Math.PI * v);
            
            currentEq = currentEq + (mu * dt) + (sigma * sqrtDt * Z);
            
            rawEq.push(currentEq);
            rawUpper.push(currentEq + 12.5 + Math.random() * 2.0);
            rawLower.push(currentEq - 12.5 - Math.random() * 2.0);
        }
        
        self.postMessage({ type: 'MASTER_SYNTHESIZED', payload: [rawTs, rawEq, rawUpper, rawLower] });
    }
    
    if (type === 'SYNTHESIZE_HAWKES') {
        let z_matrix = [];
        let x_vals = [];
        let y_vals = [];
        
        for (let x = 0; x < 5000; x++) {
            x_vals.push(x);
        }
        for (let y = 0; y < 100; y++) {
            let mappedY = -5.0 + (y / 100) * 10.0;
            y_vals.push(mappedY);
            let row = [];
            for (let x = 0; x < 5000; x++) {
                let intensity = Math.random() * 2.0; 
                let spread = Math.exp(-0.5 * (mappedY * mappedY) / 0.5); 
                row.push(intensity * spread);
            }
            z_matrix.push(row);
        }
        
        self.postMessage({ type: 'HAWKES_SYNTHESIZED', payload: { x_vals, y_vals, z_matrix } });
    }
};
