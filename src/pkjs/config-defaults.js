// Shared between config.js (Clay schema default) and index.js (fallback when
// no setting has been saved yet) so the two can't drift apart.
module.exports = {
  DEFAULT_SERVER_URL: 'http://127.0.0.1:47225/items'
};
