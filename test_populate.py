import app, json
with app.app.app_context():
    print(app.populate_test_data()[0].json)
