const WebSocket = require('ws');
const http = require('http');
const express = require('express');

const app = express();
const server = http.createServer(app);
const wss = new WebSocket.Server({ 
    server,
    // For example's sake:
    clientTracking: true,
    handleProtocols: () => 'arduino',
    perMessageDeflate: false
});

// Serve a basic HTML page (optional)
app.get('/', (req, res) => {
    res.sendFile(__dirname + '/index.html');
});

// Track active clients
const clients = new Set();

// Handle new WebSocket connections
wss.on('connection', (ws, req) => {
    console.log('New client connected from:', req.socket.remoteAddress);
    clients.add(ws);

    // Send immediate acknowledgement in JSON
    ws.send(JSON.stringify({ type: 'connection', status: 'connected' }));

    // Handle messages from this client
    ws.on('message', (message) => {
        try {
            // Attempt to parse as JSON
            const data = JSON.parse(message);
            console.log('Received (JSON):', data);

            // Broadcast the JSON data to all connected clients
            for (const client of clients) {
                if (client.readyState === WebSocket.OPEN) {
                    client.send(JSON.stringify(data));
                }
            }
        } catch (error) {
            console.error('Error processing incoming message:', error);
            console.log('Raw message was:', message.toString());
            // We could choose to ignore or handle non-JSON messages here
        }
    });

    // Handle client disconnection
    ws.on('close', () => {
        console.log('Client disconnected');
        clients.delete(ws);
    });

    // Handle WebSocket errors
    ws.on('error', (error) => {
        console.error('WebSocket error:', error);
    });
});

// Start the server
const PORT = 3015;
server.listen(PORT, '0.0.0.0', () => {
    console.log(`Server is running on http://localhost:${PORT}`);
});