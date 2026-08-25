import sqlite3

from fastapi import FastAPI
from pydantic import BaseModel

app = FastAPI()

DATABASE = "transvani.db"


class TransformerReading(BaseModel):
    transformer_id: str
    score: int
    category: str
    trend: str


def get_connection():
    connection = sqlite3.connect(DATABASE)
    connection.row_factory = sqlite3.Row
    return connection


def initialize_database():
    connection = get_connection()

    connection.execute("""
        CREATE TABLE IF NOT EXISTS readings (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            transformer_id TEXT NOT NULL,
            score INTEGER NOT NULL,
            category TEXT NOT NULL,
            trend TEXT NOT NULL
        )
        """)

    connection.commit()
    connection.close()


initialize_database()


@app.get("/")
def root():
    return {"message": "TRANSVANI backend is running"}


@app.get("/health")
def health():
    return {"status": "ok"}


@app.post("/api/readings")
def create_reading(reading: TransformerReading):
    connection = get_connection()

    connection.execute(
        """
        INSERT INTO readings (
            transformer_id,
            score,
            category,
            trend
        )
        VALUES (?, ?, ?, ?)
        """,
        (reading.transformer_id, reading.score, reading.category, reading.trend),
    )

    connection.commit()
    connection.close()

    return {"message": "Reading stored successfully"}


@app.get("/api/readings")
def get_readings():
    connection = get_connection()

    rows = connection.execute("""
        SELECT *
        FROM readings
        ORDER BY id DESC
        """).fetchall()

    connection.close()

    return [dict(row) for row in rows]


@app.get("/api/transformers")
def get_transformers():
    connection = get_connection()

    rows = connection.execute("""
        SELECT r.*
        FROM readings r
        INNER JOIN (
            SELECT transformer_id, MAX(id) AS max_id
            FROM readings
            GROUP BY transformer_id
        ) latest
        ON r.id = latest.max_id
        ORDER BY score ASC
        """).fetchall()

    connection.close()

    return [dict(row) for row in rows]